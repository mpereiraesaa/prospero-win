/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Native PE32 runtime entry. The Windows image remains private staged input;
 * this executable supplies only independently implemented ABI services. */
#include "../src/pe_image.h"
#include "../src/pe_resource.h"
#include "../src/pw_map.h"
#include "../src/pw_ini.h"
#include "../src/pw_x86_engine.h"
#include "../src/pw_vm_posix.h"
#include "../src/pw_win32.h"
#include "pw_audio_ps5.h"
#include "pw_file_ps5.h"
#include "pw_pad_ps5.h"
#include "pw_prefix_ps5.h"
#include "pw_state_ps5.h"
#include "pw_videoout_ps5.h"
#include "../src/pw_app_profile.h"
#include "../src/pw_runtime_supervisor.h"
#include "ps5log/ps5log.h"
#include <errno.h>
#include <signal.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#if !defined(MAP_ANONYMOUS) && defined(MAP_ANON)
#define MAP_ANONYMOUS MAP_ANON
#endif
#define PW_TITLE_ID "PPSA99995"
#define PW_APP_NAME "prospero-win"
#define PW_REGISTRY_PATH "/download0/prospero-win-registry.pwrg"
#ifndef PW_STAGE_DIR
#define PW_STAGE_DIR "/app0/win"
#endif
#ifndef PW_ROOT_MODULE
#define PW_ROOT_MODULE "pinball.exe"
#endif
#ifndef PW_USE_APP_PROFILE
#define PW_USE_APP_PROFILE 0
#endif
#ifndef PW_TEST_EXIT_AFTER_MS
#define PW_TEST_EXIT_AFTER_MS 0
#endif
#ifndef PW_DBT_CHAINING
#define PW_DBT_CHAINING 1
#endif
#ifndef PW_DBT_RESIDENCY
#define PW_DBT_RESIDENCY 1
#endif
#ifndef PW_DBT_LAZY_FLAGS
#define PW_DBT_LAZY_FLAGS 1
#endif

typedef struct NativeServices {
    const PeImage *image;
    const PeLayout *layout;
    PwFilePs5 *files;
    PwAudioPs5 *audio;
    PwUser32 *user32;
    uint8_t *profile_buffer;
    uint32_t profile_capacity;
    uint64_t waits;
    uint64_t profile_lookups,profile_missing,profile_errors,profile_bytes;
} NativeServices;

static volatile sig_atomic_t shutdown_requested;

static void *scratch(size_t bytes)
{
    void *result=mmap(NULL,bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    return result==MAP_FAILED?NULL:result;
}
static uint64_t now_ns(void)
{
    struct timespec value;if(clock_gettime(CLOCK_MONOTONIC,&value))return 0;
    return (uint64_t)value.tv_sec*1000000000ull+(uint64_t)value.tv_nsec;
}
static void fatal_signal(int number,siginfo_t *info,void *context)
{
    const ucontext_t *uc=context;
    PS5LOG_LOG("PW_RUNTIME_SIGNAL sig=%d code=%d addr=%p pc=%p rax=%p rsp=%p",
        number,info?info->si_code:0,info?info->si_addr:NULL,
        uc?(void *)(uintptr_t)uc->uc_mcontext.mc_rip:NULL,
        uc?(void *)(uintptr_t)uc->uc_mcontext.mc_rax:NULL,
        uc?(void *)(uintptr_t)uc->uc_mcontext.mc_rsp:NULL);
    ps5log_close("runtime-signal");_exit(1);
}
static void shutdown_signal(int number)
{
    (void)number;shutdown_requested=1;
}
static void install_signals(void)
{
    static const int values[]={SIGSEGV,SIGBUS,SIGILL,SIGFPE,SIGABRT,SIGTRAP,SIGSYS};
    struct sigaction action;memset(&action,0,sizeof(action));
    action.sa_sigaction=fatal_signal;action.sa_flags=SA_SIGINFO|SA_RESETHAND;
    for(unsigned i=0;i<sizeof(values)/sizeof(values[0]);i++)
        (void)sigaction(values[i],&action,NULL);
    struct sigaction orderly;memset(&orderly,0,sizeof(orderly));
    orderly.sa_handler=shutdown_signal;
    (void)sigaction(SIGINT,&orderly,NULL);(void)sigaction(SIGTERM,&orderly,NULL);
}
static int source_view(void *opaque,uint32_t pc,const uint8_t **source,size_t *bytes)
{
    const NativeServices *services=opaque;const PeImage *image=services->image;
    if(pc<image->image_base)return PW_ERR_NOT_FOUND;
    uint32_t rva=pc-(uint32_t)image->image_base;
    for(unsigned i=0;i<services->layout->section_count;i++) {
        const PeLayoutSection *section=&services->layout->sections[i];
        uint64_t end=(uint64_t)section->rva+section->mapped_bytes;
        if((section->protection&PW_PROT_EXEC) && rva>=section->rva && rva<end) {
            *source=(const uint8_t *)(uintptr_t)pc;*bytes=(size_t)(end-rva);return PW_OK;
        }
    }
    return PW_ERR_NOT_FOUND;
}
static int string_resource(void *opaque,uint32_t module,uint32_t id,
                           const uint8_t **text,size_t *units)
{
    const NativeServices *s=opaque;
    if(module && module!=s->image->image_base)return PW_ERR_UNSUPPORTED;
    return pe_resource_string(s->image,id,0x409,text,units);
}
static int named_resource(void *opaque,uint32_t module,uint32_t type,const char *name,
                          const uint8_t **bytes,size_t *size)
{
    const NativeServices *s=opaque;PeResource resource;
    if(module!=s->image->image_base)return PW_ERR_UNSUPPORTED;
    int status=pe_resource_find_name(s->image,type,name,0x409,&resource);
    if(status==PW_OK){*bytes=resource.bytes;*size=resource.size;}return status;
}
static int integer_resource(void *opaque,uint32_t module,uint32_t type,uint32_t name,
                            const uint8_t **bytes,size_t *size)
{
    const NativeServices *s=opaque;PeResource resource;
    if(module!=s->image->image_base)return PW_ERR_UNSUPPORTED;
    int status=pe_resource_find(s->image,type,name,0x409,&resource);
    if(status==PW_OK){*bytes=resource.bytes;*size=resource.size;}return status;
}
static int code_address(void *opaque,uint32_t address)
{
    const uint8_t *source;size_t bytes;return source_view(opaque,address,&source,&bytes);
}
static int clock_ns(void *opaque,PwClockDomain domain,uint64_t *value)
{
    (void)opaque;clockid_t id=domain==PW_CLOCK_UTC?CLOCK_REALTIME:CLOCK_MONOTONIC;
    if(domain!=PW_CLOCK_UTC && domain!=PW_CLOCK_UPTIME && domain!=PW_CLOCK_COUNTER)
        return PW_ERR_UNSUPPORTED;
    struct timespec now;if(clock_gettime(id,&now))return PW_ERR_STATE;
    *value=(uint64_t)now.tv_sec*1000000000ull+(uint64_t)now.tv_nsec;return PW_OK;
}
static int file_open(void *opaque,const char *path,const char *mode,uint32_t *handle)
{return pw_file_ps5_stream_open(((NativeServices *)opaque)->files,path,mode,handle);}
static int file_close(void *opaque,uint32_t handle)
{return pw_file_ps5_stream_close(((NativeServices *)opaque)->files,handle);}
static int file_read(void *opaque,uint32_t handle,void *output,uint32_t bytes,uint32_t *got)
{return pw_file_ps5_stream_read(((NativeServices *)opaque)->files,handle,output,bytes,got);}
static int file_seek(void *opaque,uint32_t handle,int32_t offset,uint32_t origin,uint32_t *position)
{return pw_file_ps5_stream_seek(((NativeServices *)opaque)->files,handle,offset,origin,position);}
static int profile_int(void *opaque,const char *section,const char *key,uint32_t fallback,
                       const char *filename,uint32_t *value)
{
    NativeServices *services=opaque;if(!services || !section || !key || !filename || !value)
        return PW_ERR_PRECONDITION;
    services->profile_lookups++;*value=fallback;uint32_t handle=0;
    int status=pw_file_ps5_stream_open(services->files,filename,"rb",&handle);
    if(status==PW_ERR_NOT_FOUND){services->profile_missing++;return PW_OK;}
    if(status!=PW_OK){services->profile_errors++;return status;}
    uint32_t total=0;
    while(total<services->profile_capacity) {
        uint32_t got=0;status=pw_file_ps5_stream_read(services->files,handle,
            services->profile_buffer+total,services->profile_capacity-total,&got);
        if(status!=PW_OK || !got)break;total+=got;
    }
    if(status==PW_OK && total==services->profile_capacity) {
        uint8_t extra;uint32_t got=0;
        status=pw_file_ps5_stream_read(services->files,handle,&extra,1,&got);
        if(status==PW_OK && got)status=PW_ERR_LIMIT;
    }
    int close_status=pw_file_ps5_stream_close(services->files,handle);
    if(status==PW_OK && close_status!=PW_OK)status=close_status;
    if(status==PW_OK) {
        services->profile_bytes+=total;
        status=pw_ini_get_int(services->profile_buffer,total,section,key,fallback,value);
    }
    if(status!=PW_OK)services->profile_errors++;
    return status;
}
static int message_wait(void *opaque,uint32_t window,PwUser32QueueEntry *message)
{
    NativeServices *services=opaque;if(!window || !message)return PW_ERR_PRECONDITION;
    (void)usleep(16667);uint64_t phase=services->waits++;
    *message=(PwUser32QueueEntry){.window=window};
    if(phase==0) {
        /* A shown top-level window gains focus on Win32. Start a normal game,
         * but leave every gameplay edge to the physical input adapter. */
        message->message=0x0007; /* WM_SETFOCUS */
        PwUser32QueueEntry new_game={.window=window,.message=0x0111,.wparam=101};
        int status=pw_user32_post_message(services->user32,&new_game);
        if(status!=PW_OK)return status;
    }
    return PW_OK;
}
static int sleep_ms(void *opaque,uint32_t milliseconds)
{
    (void)opaque;
    while(milliseconds) {
        uint32_t chunk=milliseconds>1000?1000:milliseconds;
        if(usleep(chunk*1000u) && errno!=EINTR)return PW_ERR_STATE;
        milliseconds-=chunk;
    }
    return PW_OK;
}
static int audio_open(void *opaque,uint32_t rate,uint16_t channels,uint16_t bits)
{
    int status=pw_audio_ps5_open(((NativeServices *)opaque)->audio,rate,channels,bits);
    PS5LOG_LOG("PW_AUDIO_OPEN status=%s rate=%u channels=%u bits=%u",
               pw_result_name(status),rate,channels,bits);return status;
}
static int audio_submit(void *opaque,const void *pcm,uint32_t bytes,uint32_t token)
{
    return pw_audio_ps5_submit(((NativeServices *)opaque)->audio,pcm,bytes,token);
}
static int audio_poll(void *opaque,uint32_t *token,uint32_t *bytes)
{return pw_audio_ps5_poll(((NativeServices *)opaque)->audio,token,bytes);}
static int audio_control(void *opaque,PwAudioControl control)
{return pw_audio_ps5_control(((NativeServices *)opaque)->audio,control);}

static void abort_runtime(const char *stage,int status)
{
    PS5LOG_LOG("PW_RUNTIME_ABORT stage=%s status=%s",stage,pw_result_name(status));
    ps5log_close(stage);_exit(1);
}

static int app_profile_load(PwFilePs5 *files, uint8_t *buffer,
                            PwAppProfile *profile)
{
    uint32_t handle=0,total=0;
    int status=pw_file_ps5_stream_open(files,"app.profile","rb",&handle);
    if(status!=PW_OK)return status;
    while(total<PW_APP_PROFILE_MAX_BYTES) {
        uint32_t got=0;
        status=pw_file_ps5_stream_read(files,handle,buffer+total,
            PW_APP_PROFILE_MAX_BYTES-total,&got);
        if(status!=PW_OK || !got)break;
        total+=got;
    }
    if(status==PW_OK && total==PW_APP_PROFILE_MAX_BYTES) {
        uint8_t extra;uint32_t got=0;
        status=pw_file_ps5_stream_read(files,handle,&extra,1,&got);
        if(status==PW_OK && got)status=PW_ERR_LIMIT;
    }
    int close_status=pw_file_ps5_stream_close(files,handle);
    if(status==PW_OK && close_status!=PW_OK)status=close_status;
    if(status!=PW_OK)return status;
    return pw_app_profile_parse(buffer,total,profile);
}

static int app_profile_stage_name(const char *path,char *out,size_t capacity)
{
    const char *basename=strrchr(path,'\\');
    basename=basename?basename+1:path;
    size_t length=strlen(basename);
    if(!length || length>=capacity)return PW_ERR_LIMIT;
    for(size_t index=0;index<length;index++) {
        unsigned char value=(unsigned char)basename[index];
        out[index]=(char)(value>='A' && value<='Z'?value+('a'-'A'):value);
    }
    out[length]='\0';
    return PW_OK;
}

static int app_profile_commandline(const PwAppProfile *profile,char *out,
                                   size_t capacity)
{
    size_t used=0;
    const char *parts[]={"\"",profile->executable,"\""};
    for(unsigned part=0;part<3;part++) {
        size_t length=strlen(parts[part]);
        if(length>=capacity-used)return PW_ERR_LIMIT;
        memcpy(out+used,parts[part],length);used+=length;
    }
    if(profile->arguments[0]) {
        size_t length=strlen(profile->arguments);
        if(used+1u+length>=capacity)return PW_ERR_LIMIT;
        out[used++]=' ';
        memcpy(out+used,profile->arguments,length);used+=length;
    }
    out[used]='\0';
    return PW_OK;
}

static int prefix_registry_path(const PwPrefixLayout *prefix,char *out,
                                size_t capacity)
{
    static const char suffix[]="/registry.pwrg";
    size_t root_length;
    if(!prefix || !out)return PW_ERR_PRECONDITION;
    root_length=strlen(prefix->root);
    if(root_length+sizeof(suffix)>capacity)return PW_ERR_LIMIT;
    memcpy(out,prefix->root,root_length);
    memcpy(out+root_length,suffix,sizeof(suffix));
    return PW_OK;
}

static void collect_cleanup_status(int *overall,int status)
{
    if(*overall==PW_OK && status!=PW_OK)*overall=status;
}

static void report_execute_abort(const NativeServices *services,
                                 const PwX86State *state,int status)
{
    const uint8_t *source=NULL;size_t available=0;
    int view_status=source_view((void *)services,state->eip,&source,&available);
    uint8_t bytes[8]={0};size_t copied=0;
    if(view_status==PW_OK && source) {
        copied=available<sizeof(bytes)?available:sizeof(bytes);
        memcpy(bytes,source,copied);
    }
    PS5LOG_LOG("PW_EXEC_ABORT schema=1 status=%s eip=0x%08x "
        "eax=0x%08x ecx=0x%08x edx=0x%08x ebx=0x%08x "
        "esp=0x%08x ebp=0x%08x esi=0x%08x edi=0x%08x eflags=0x%08x "
        "view=%s available=%llu captured=%u "
        "bytes=%02x%02x%02x%02x%02x%02x%02x%02x",
        pw_result_name(status),state->eip,state->gpr[0],state->gpr[1],
        state->gpr[2],state->gpr[3],state->gpr[4],state->gpr[5],
        state->gpr[6],state->gpr[7],state->eflags,pw_result_name(view_status),
        (unsigned long long)available,(unsigned)copied,bytes[0],bytes[1],bytes[2],
        bytes[3],bytes[4],bytes[5],bytes[6],bytes[7]);
}

enum { PAD_CREATE=0x00000001u,PAD_OPTIONS=0x00000008u,PAD_UP=0x00000010u,
       PAD_RIGHT=0x00000020u,PAD_LEFT=0x00000080u,PAD_L1=0x00000400u,
       PAD_R1=0x00000800u,PAD_CROSS=0x00004000u,PAD_SQUARE=0x00008000u };
static const PwPadKeyMap pinball_pad_map[]={
    {PAD_L1,'Z',0,0,"left-flipper"},
    {PAD_R1,0xbf,0,0,"right-flipper"},
    {PAD_CROSS,0x20,0,0,"plunger"},
    {PAD_LEFT,'X',0,0,"nudge-left"},
    {PAD_RIGHT,0xbe,0,0,"nudge-right"},
    {PAD_UP,0x26,0x48,1,"nudge-up"},
    {PAD_OPTIONS,0x72,0,0,"pause"},
    {PAD_SQUARE,0x71,0,0,"new-game"},
};

static uint32_t presentation_window(const PwUser32 *user,const PwGdi *gdi,
                                    PwGdiTargetView *view)
{
    if(user->focus_window &&
       pw_gdi_target_view(gdi,user->focus_window,view)==PW_OK &&
       view->width>=600 && view->height>=400)return user->focus_window;
    uint32_t owner=0;memset(view,0,sizeof(*view));
    for(uint32_t i=0;i<user->window_capacity;i++)if(user->windows[i].used &&
       !user->windows[i].creating && user->windows[i].visible) {
        PwGdiTargetView candidate;
        if(pw_gdi_target_view(gdi,user->windows[i].handle,&candidate)==PW_OK &&
           candidate.width>=600 && candidate.height>=400 &&
           candidate.width*candidate.height>view->width*view->height) {
            *view=candidate;owner=user->windows[i].handle;
        }
    }
    return owner;
}

int main(int argc,char **argv)
{
    (void)argc;(void)argv;ps5log_config config;const char *config_path=NULL;
    ps5log_config_defaults(&config);int config_rc=ps5log_load_config(ps5log_default_conf_paths,
        ps5log_default_conf_path_count,&config,&config_path);
    int log_rc=config_rc==0?ps5log_init(&config,PW_TITLE_ID,PW_APP_NAME,now_ns()):config_rc;
    install_signals();PS5LOG_LOG("PW_RUNTIME_BEGIN schema=1 title=%s profile_mode=%u fallback_root=%s config=%d log=%d",
        PW_TITLE_ID,(unsigned)PW_USE_APP_PROFILE,PW_ROOT_MODULE,config_rc,log_rc);

    PwFilePs5 *files=scratch(sizeof(*files));PwFileProvider provider;PwFileSpan root={0};
    if(!files)abort_runtime("files-scratch",PW_ERR_VM);
    int status=pw_file_ps5_init(files,PW_STAGE_DIR);if(status!=PW_OK)abort_runtime("files",status);
    (void)pw_file_ps5_provider(files,&provider);
    PwAppProfile app_profile;memset(&app_profile,0,sizeof(app_profile));
    const char *root_module=PW_ROOT_MODULE;
    const char *registry_path=PW_REGISTRY_PATH;
    char profile_module[PW_APP_PATH_CAPACITY];
    char profile_registry_path[PW_PREFIX_PATH_CAPACITY];
    PwPrefixLayout *prefix_layout=NULL;
    PwRuntimeSupervisor *supervisor=NULL;
    if(PW_USE_APP_PROFILE) {
        uint8_t *profile_bytes=scratch(PW_APP_PROFILE_MAX_BYTES);
        if(!profile_bytes)abort_runtime("profile-scratch",PW_ERR_VM);
        status=app_profile_load(files,profile_bytes,&app_profile);
        if(status!=PW_OK)abort_runtime("app-profile",status);
        if(app_profile.architecture!=PW_APP_ARCH_PE32 ||
           app_profile.graphics!=PW_APP_GRAPHICS_GDI)
            abort_runtime("profile-capability",PW_ERR_UNSUPPORTED);
        status=app_profile_stage_name(app_profile.executable,profile_module,
                                      sizeof(profile_module));
        if(status!=PW_OK)abort_runtime("profile-module-name",status);
        root_module=profile_module;
        PwPrefixIo prefix_io;PwPrefixService prefix_service;
        prefix_layout=scratch(sizeof(*prefix_layout));
        supervisor=scratch(sizeof(*supervisor));
        if(!prefix_layout || !supervisor)abort_runtime("profile-state-scratch",PW_ERR_VM);
        if((status=pw_prefix_ps5_io(&prefix_io))!=PW_OK ||
           (status=pw_prefix_service_init(&prefix_service,
               PW_PREFIX_PS5_DEFAULT_ROOT,&prefix_io))!=PW_OK ||
           (status=pw_prefix_open(&prefix_service,app_profile.prefix,
               prefix_layout))!=PW_OK)
            abort_runtime("prefix-open",status);
        if((status=prefix_registry_path(prefix_layout,profile_registry_path,
            sizeof(profile_registry_path)))!=PW_OK)
            abort_runtime("prefix-registry-path",status);
        registry_path=profile_registry_path;
        pw_runtime_supervisor_init(supervisor);
        if((status=pw_runtime_supervisor_begin(supervisor,&app_profile,
            prefix_layout))!=PW_OK)
            abort_runtime("runtime-session-begin",status);
        PS5LOG_LOG("PW_PREFIX_OPEN schema=1 id=%s root=%s registry=%s",
            prefix_layout->id,prefix_layout->root,registry_path);
        PS5LOG_LOG("PW_APP_PROFILE schema=1 id=%s runtime=%s prefix=%s exe=%s cwd=%s arch=pe32 graphics=gdi arguments=%u",
            app_profile.id,app_profile.runtime,app_profile.prefix,
            app_profile.executable,app_profile.working_directory,
            (unsigned)(app_profile.arguments[0]!=0));
    }
    status=provider.open(provider.context,root_module,&root);if(status!=PW_OK)abort_runtime("root",status);
    PeImage image;if((status=pe_image_parse(&image,root.bytes,root.size))!=PW_OK ||
       image.machine!=PE_MACHINE_I386)abort_runtime("pe32",status);

    PwVmBackend vm;PwVmRegion stack={0},thread={0},crt={0},heap_region={0};
    if((status=pw_vm_posix_backend(&vm))!=PW_OK)abort_runtime("vm",status);
    if((status=vm.reserve_at(NULL,0x03000000,0x100000,4096,&stack))!=PW_OK ||
       (status=vm.reserve_at(NULL,0x03200000,4096,4096,&thread))!=PW_OK ||
       (status=vm.reserve_at(NULL,0x03300000,4096,4096,&crt))!=PW_OK ||
       (status=vm.reserve_at(NULL,0x03400000,0x4000000,4096,&heap_region))!=PW_OK)
        abort_runtime("guest-reserve",status);
    if((status=vm.commit(NULL,&stack,0,stack.bytes,PW_PROT_READ|PW_PROT_WRITE))!=PW_OK ||
       (status=vm.commit(NULL,&thread,0,thread.bytes,PW_PROT_READ|PW_PROT_WRITE))!=PW_OK ||
       (status=vm.commit(NULL,&crt,0,crt.bytes,PW_PROT_READ|PW_PROT_WRITE))!=PW_OK ||
       (status=vm.commit(NULL,&heap_region,0,heap_region.bytes,PW_PROT_READ|PW_PROT_WRITE))!=PW_OK)
        abort_runtime("guest-commit",status);

    PeLayout layout;PwMappedImage mapped={0};PwMapVerify verify;
    if((status=pe_layout_plan(&layout,&image))!=PW_OK ||
       (status=pw_map_image(&mapped,&image,&layout,&vm))!=PW_OK ||
       (status=pw_map_verify(&mapped,&image,&layout,&verify))!=PW_OK ||
       mapped.actual_base!=image.image_base || verify.raw_mismatches ||
       verify.zero_tail_violations || verify.alias_mismatches)
        abort_runtime("image-map",status);

    PwHeapBlock *heap_blocks=scratch(131072u*sizeof(*heap_blocks));
    PwRegistryKey *registry_keys=scratch(32u*sizeof(*registry_keys));
    PwRegistryValue *registry_values=scratch(128u*sizeof(*registry_values));
    uint8_t *state_buffer=scratch(PW_STATE_PS5_MAX_BYTES);
    uint8_t *profile_buffer=scratch(64u*1024u);
    PwUser32Message *messages=scratch(128u*sizeof(*messages));
    PwUser32Window *windows=scratch(128u*sizeof(*windows));
    PwUser32Resource *resources=scratch(128u*sizeof(*resources));
    PwUser32Class *classes=scratch(128u*sizeof(*classes));
    PwGdiDc *dcs=scratch(128u*sizeof(*dcs));PwGdiSurface *surfaces=scratch(128u*sizeof(*surfaces));
    uint8_t *pixels=scratch(32u*1024u*1024u);
    PwX86CacheEntry *cache=scratch(8192u*sizeof(*cache));
    if(!heap_blocks||!registry_keys||!registry_values||!state_buffer||!profile_buffer||!messages||!windows||!resources||
       !classes||!dcs||!surfaces||!pixels||!cache)abort_runtime("runtime-scratch",PW_ERR_VM);
    PwGuestHeap heap;PwRegistry registry;PwUser32 user32;PwGdi gdi;
    if((status=pw_guest_heap_init(&heap,0x03400000,0x4000000,heap_blocks,131072))!=PW_OK ||
       (status=pw_registry_init(&registry,registry_keys,32,registry_values,128))!=PW_OK ||
       (status=pw_user32_init(&user32,messages,128,windows,128))!=PW_OK ||
       (status=pw_user32_init_resources(&user32,resources,128))!=PW_OK ||
       (status=pw_user32_init_classes(&user32,classes,128))!=PW_OK ||
       (status=pw_user32_configure_desktop(&user32,1920,1080))!=PW_OK ||
       (status=pw_gdi_init(&gdi,dcs,128,surfaces,128,pixels,32u*1024u*1024u))!=PW_OK)
        abort_runtime("win32-state",status);
    uint32_t state_loaded=0;
    status=pw_state_ps5_load_registry(&registry,registry_path,state_buffer,
                                      PW_STATE_PS5_MAX_BYTES,&state_loaded);
    PS5LOG_LOG("PW_STATE_LOAD schema=1 status=%s bytes=%u path=%s",
               pw_result_name(status),state_loaded,registry_path);
    if(status!=PW_OK)abort_runtime("state-load",status);

    PwAudioPs5 audio;PwAudioPs5Ops audio_ops;
    PwAudioPs5Block *audio_queue=scratch(PW_AUDIO_PS5_QUEUE_BLOCKS*sizeof(*audio_queue));
    if(!audio_queue)abort_runtime("audio-queue",PW_ERR_VM);
    if((status=pw_audio_ps5_platform_ops(&audio_ops))!=PW_OK ||
       (status=pw_audio_ps5_init(&audio,&audio_ops,audio_queue,
                                 PW_AUDIO_PS5_QUEUE_BLOCKS))!=PW_OK)
        abort_runtime("audio",status);
    PwPadPs5 pad;PwPadPs5Ops pad_ops;
    if((status=pw_pad_ps5_platform_ops(&pad_ops))!=PW_OK ||
       (status=pw_pad_ps5_open(&pad,&pad_ops,pinball_pad_map,
        sizeof(pinball_pad_map)/sizeof(pinball_pad_map[0])))!=PW_OK)
        abort_runtime("pad",status);
    PS5LOG_LOG("PW_PAD_OPEN schema=1 user_service_rc=%d owns_user_service=%u user=%d pad_init_rc=%d handle=%d read=scePadRead batch=%u",
        pad.user_initialize_rc,pad.owns_user_service,pad.user_id,pad.pad_init_rc,
        pad.pad_handle,PW_PAD_PS5_BATCH);
    NativeServices services={.image=&image,.layout=&layout,.files=files,.audio=&audio,
        .user32=&user32,.profile_buffer=profile_buffer,.profile_capacity=64u*1024u};
    PwWin32 runtime;char commandline[1024];
    if(PW_USE_APP_PROFILE) {
        status=app_profile_commandline(&app_profile,commandline,sizeof(commandline));
        if(status!=PW_OK)abort_runtime("profile-commandline",status);
    } else {
        const char legacy_commandline[]="\"C:\\game\\" PW_ROOT_MODULE "\"";
        if(sizeof(legacy_commandline)>sizeof(commandline))
            abort_runtime("legacy-commandline",PW_ERR_LIMIT);
        memcpy(commandline,legacy_commandline,sizeof(legacy_commandline));
    }
    if((status=pw_win32_init(&runtime,(uint32_t)mapped.actual_base,0x03300000,commandline))!=PW_OK)
        abort_runtime("win32-init",status);
    runtime.heap=&heap;runtime.registry=&registry;runtime.user32=&user32;runtime.gdi=&gdi;
    runtime.services=(PwWin32Services){.opaque=&services,.clock_ns=clock_ns,
        .process_id=1,.thread_id=2,.string_resource=string_resource,
        .named_resource=named_resource,.integer_resource=integer_resource,
        .code_address=code_address,.ansi_codepage=1252,
        .main_module_filename=PW_USE_APP_PROFILE?app_profile.executable:
            "C:\\game\\" PW_ROOT_MODULE,
        .file_open=file_open,.file_close=file_close,.file_read=file_read,.file_seek=file_seek,
        .profile_int=profile_int,.message_wait=message_wait,.sleep_ms=sleep_ms,.audio_open=audio_open,
        .audio_submit=audio_submit,.audio_poll=audio_poll,.audio_control=audio_control};
    PwImportBindWorkspace *workspace=scratch(sizeof(*workspace));PwImportBindReport binding;
    if(!workspace || (status=pw_import_bind32(&image,&mapped,pw_win32_resolve,&runtime,
       workspace,&binding))!=PW_OK)abort_runtime("imports",status);
    if((status=pw_map_finalize_protections(&mapped,&layout,&vm))!=PW_OK)
        abort_runtime("protect",status);

    PwX86State state;memset(&state,0,sizeof(state));pw_guest_fp_init(&state.fp);
    state.memory[state.memory_count++]=(PwX86Memory){(uint32_t)mapped.actual_base,
        mapped.actual_base+layout.header_bytes,PW_X86_READ};
    for(unsigned i=0;i<layout.section_count;i++) {
        const PeLayoutSection *section=&layout.sections[i];
        state.memory[state.memory_count++]=(PwX86Memory){(uint32_t)mapped.actual_base+section->rva,
            mapped.actual_base+section->rva+section->mapped_bytes,
            ((section->protection&PW_PROT_READ)?PW_X86_READ:0)|
            ((section->protection&PW_PROT_WRITE)?PW_X86_WRITE:0)};
    }
    state.memory[state.memory_count++]=(PwX86Memory){0x03300000,0x03301000,PW_X86_READ|PW_X86_WRITE};
    state.memory[state.memory_count++]=(PwX86Memory){0x03400000,0x07400000,PW_X86_READ|PW_X86_WRITE};
    state.stack_low=0x03000000;state.stack_high=0x03100000;state.gpr[4]=state.stack_high-4;
    state.fs_base=0x03200000;state.fs_bytes=4096;state.eflags=0x202;
    state.eip=(uint32_t)image.image_base+image.entry_point;*(uint32_t *)thread.write_base=0xffffffffu;
    PwX86Engine engine={0};
    if((status=pw_x86_engine_init(&engine,&vm,cache,8192,4u*1024u*1024u,1,
                                  source_view,&services))!=PW_OK)abort_runtime("dbt",status);
    if(pw_x86_engine_set_chaining(&engine,PW_DBT_CHAINING)!=PW_OK ||
       pw_x86_engine_set_residency(&engine,PW_DBT_RESIDENCY)!=PW_OK ||
       pw_x86_engine_set_lazy_flags(&engine,PW_DBT_LAZY_FLAGS)!=PW_OK)
        abort_runtime("dbt-mode",PW_ERR_STATE);
    PwVideoOutPs5 video;
    if((status=pw_videoout_ps5_open(&video))!=PW_OK)abort_runtime("videoout",status);
    if(supervisor &&
       (status=pw_runtime_supervisor_guest_started(supervisor))!=PW_OK)
        abort_runtime("runtime-session-start",status);
    PS5LOG_LOG("PW_RUNTIME_READY imports=%u entry=0x%08x image_bytes=%u "
               "dbt_chaining=%u dbt_residency=%u dbt_lazy_flags=%u dbt_quantum=%u",
               binding.total,state.eip,image.size_of_image,PW_DBT_CHAINING,
               PW_DBT_RESIDENCY,PW_DBT_LAZY_FLAGS,engine.quantum);
    uint64_t events=0,last_heartbeat=now_ns(),last_present=0,last_pad_poll=0;
    uint64_t audio_completion_events=0;
    uint64_t last_loop=0,loop_gap_max_ns=0,loop_gaps_16ms=0,loop_gaps_33ms=0;
    uint64_t idle_yields=0,idle_yield_ns=0;
    uint64_t saved_generation=registry.generation,last_save_attempt=0;
    unsigned window_inventory_logged=0;
    uint32_t last_frame_hash=0;
    const uint64_t validation_deadline=PW_TEST_EXIT_AFTER_MS?
        now_ns()+(uint64_t)PW_TEST_EXIT_AFTER_MS*1000000ull:0;
    const char *exit_reason="guest-return";uint32_t exit_code=0;
    for(;;events++) {
        uint64_t loop_now=now_ns();
        if(last_loop) {
            uint64_t gap=loop_now-last_loop;
            if(gap>loop_gap_max_ns)loop_gap_max_ns=gap;
            if(gap>=16666667ull)loop_gaps_16ms++;
            if(gap>=33333333ull)loop_gaps_33ms++;
        }
        last_loop=loop_now;
        if(shutdown_requested){exit_reason="host-signal";break;}
        if(validation_deadline && now_ns()>=validation_deadline) {
            exit_reason="validation-deadline";break;
        }
        if(!state.eip){exit_reason="guest-return";break;}
        uint32_t audio_completed=0;
        if((status=pw_win32_pump_audio(&runtime,&state,&audio_completed))!=PW_OK)
            abort_runtime("audio-completion",status);
        audio_completion_events+=audio_completed;
        status=pw_win32_dispatch(&runtime,&state);
        if(status==PW_ERR_NOT_FOUND) {
            PwX86StepReport report;status=pw_x86_engine_step(&engine,&state,&report);
        }
        if(status!=PW_OK) {
            report_execute_abort(&services,&state,status);
            abort_runtime("execute",status);
        }
        if(runtime.exit_requested) {
            exit_reason="crt-exit";exit_code=runtime.exit_code;break;
        }
        if(runtime.idle_hint) {
            const uint32_t yield_us=500;
            if(usleep(yield_us) && errno!=EINTR)abort_runtime("idle-yield",PW_ERR_STATE);
            idle_yields++;idle_yield_ns+=(uint64_t)yield_us*1000u;
        }
        uint64_t now=now_ns();
        if(registry.generation!=saved_generation && now-last_save_attempt>=1000000000ull) {
            uint32_t state_written=0;PwStatePs5Report save_report;last_save_attempt=now;
            int save_status=pw_state_ps5_save_registry_ex(&registry,registry_path,state_buffer,
                PW_STATE_PS5_MAX_BYTES,&state_written,&save_report);
            PS5LOG_LOG("PW_STATE_SAVE schema=1 status=%s generation=%llu bytes=%u path=%s "
                "encoded=%u open_rc=0x%08x write_rc=%d written=%u fsync_rc=0x%08x "
                "close_rc=0x%08x rename_rc=0x%08x unlink_rc=0x%08x",
                pw_result_name(save_status),(unsigned long long)registry.generation,state_written,
                registry_path,
                save_report.encoded_bytes,(uint32_t)save_report.open_rc,save_report.write_rc,
                save_report.written_bytes,(uint32_t)save_report.fsync_rc,
                (uint32_t)save_report.close_rc,(uint32_t)save_report.rename_rc,
                (uint32_t)save_report.unlink_rc);
            if(save_status==PW_OK)saved_generation=registry.generation;
        }
        if(now-last_pad_poll>=4166667ull) {
            PwGdiTargetView input_view;uint32_t input_window=presentation_window(&user32,&gdi,&input_view);
            if(input_window) {
                uint64_t before=pad.core.stats.events;
                if((status=pw_pad_ps5_poll(&pad,&user32,input_window))!=PW_OK)
                    abort_runtime("pad-read",status);
                if(pad.core.pressed_edges&PAD_CREATE) {
                    if((status=pw_user32_post_quit(&user32,0))!=PW_OK)
                        abort_runtime("pad-quit",status);
                    PS5LOG_LOG("PW_PAD_QUIT schema=1 source=create action=WM_QUIT");
                }
                if(pad.core.stats.events!=before)
                    PS5LOG_LOG("PW_PAD_EVENT schema=1 events=%llu presses=%llu releases=%llu generation=%u timestamp_source=scePadRead",
                        (unsigned long long)pad.core.stats.events,
                        (unsigned long long)pad.core.stats.presses,
                        (unsigned long long)pad.core.stats.releases,pad.core.generation);
            }
            last_pad_poll=now;
        }
        if(now-last_present>=33333333ull) {
            PwGdiTargetView best;uint32_t owner=presentation_window(&user32,&gdi,&best);
            if(best.pixels && best.width>=600 && best.height>=400) {
                uint32_t hash=2166136261u;
                for(uint32_t y=0;y<best.height;y++) {
                    const uint8_t *row=best.pixels+(size_t)y*best.stride;
                    for(uint32_t x=0;x<best.width*4u;x++){hash^=row[x];hash*=16777619u;}
                }
                if(hash!=last_frame_hash) {
                    if((status=pw_videoout_ps5_present(&video,&best))!=PW_OK)
                        abort_runtime("present",status);
                    last_frame_hash=hash;
                    if(video.flips==1 || !(video.flips%120))
                        PS5LOG_LOG("PW_VIDEO_FRAME flips=%llu width=%u height=%u owner=0x%08x focus=0x%08x hash=0x%08x backend=agc-dma submits=%llu fence=zero",
                            (unsigned long long)video.flips,best.width,best.height,owner,user32.focus_window,hash,
                            (unsigned long long)video.agc.submits);
                }
            }
            last_present=now;
        }
        if(now-last_heartbeat>=5000000000ull) {
            PwGdiCounts counts;PwGdiTargetView view;unsigned presented=0,window_count=0;
            PwAudioPs5Stats audio_stats;
            if(pw_audio_ps5_stats(&audio,&audio_stats)!=PW_OK)
                abort_runtime("audio-stats",PW_ERR_STATE);
            (void)pw_gdi_counts(&gdi,&counts);
            for(unsigned i=0;i<128;i++)if(windows[i].used) {
                window_count++;
                if(pw_gdi_target_view(&gdi,windows[i].handle,&view)==PW_OK && view.width>=600)
                    presented=1;
                if(!window_inventory_logged) {
                    int target=pw_gdi_target_view(&gdi,windows[i].handle,&view)==PW_OK;
                    PS5LOG_LOG("PW_WINDOW schema=1 handle=0x%08x focus=%u visible=%u class=%s title=%s x=%d y=%d width=%u height=%u target=%u target_width=%u target_height=%u",
                        windows[i].handle,windows[i].handle==user32.focus_window,windows[i].visible,
                        windows[i].class_name,windows[i].title,(int32_t)windows[i].x,
                        (int32_t)windows[i].y,windows[i].width,windows[i].height,target,
                        target?view.width:0,target?view.height:0);
                }
            }
            if(!window_inventory_logged)
                for(unsigned i=0;i<128;i++)if(surfaces[i].used &&
                   surfaces[i].kind==PW_GDI_TARGET_SURFACE)
                    PS5LOG_LOG("PW_GDI_TARGET schema=1 slot=%u owner=0x%08x width=%u height=%u bytes=%u",
                        i,surfaces[i].target,surfaces[i].width,surfaces[i].height,surfaces[i].bytes);
            window_inventory_logged=1;
            PS5LOG_LOG("PW_RUNTIME_HEARTBEAT schema=2 events=%llu retired=%llu calls=%u waits=%llu "
                "dbt_dispatches=%llu dbt_compiles=%llu dbt_hits=%llu dbt_misses=%llu "
                "dbt_lookup_probes=%llu dbt_max_probe=%u dbt_protect_calls=%llu dbt_protect_bytes=%llu "
                "dbt_links=%llu dbt_linked_transitions=%llu dbt_safepoints=%llu "
                "dbt_reg_loads=%llu dbt_reg_stores=%llu dbt_reg_reconciliations=%llu dbt_reg_spills=%llu "
                "dbt_flags_safepoint_commits=%llu "
                "windows=%u targets=%u visible_source=%u flips=%llu audio_blocks=%llu "
                "audio_bytes=%llu audio_frames=%llu audio_hash=0x%08x "
                "audio_enqueues=%llu audio_completions=%llu audio_queue=%u "
                "audio_queue_high_water=%u audio_queue_full=%llu audio_errors=%llu "
                "pad_polls=%llu pad_samples=%llu pad_events=%llu pad_connected=%llu "
                "pad_intercepted=%llu pad_read_errors=%llu profile_lookups=%llu "
                "profile_missing=%llu profile_errors=%llu profile_bytes=%llu "
                "mci_calls=%llu mci_last_command=0x%08x "
                "idle_yields=%llu idle_yield_ns=%llu loop_gap_max_ns=%llu "
                "loop_gaps_16ms=%llu loop_gaps_33ms=%llu",
                (unsigned long long)events,(unsigned long long)engine.retired_instructions,
                runtime.calls,(unsigned long long)services.waits,
                (unsigned long long)engine.dispatches,(unsigned long long)engine.compiles,
                (unsigned long long)engine.cache.hits,(unsigned long long)engine.cache.misses,
                (unsigned long long)engine.cache.lookup_probes,engine.cache.max_probe,
                (unsigned long long)engine.protection_calls,
                (unsigned long long)engine.protection_bytes,
                (unsigned long long)engine.successful_links,
                (unsigned long long)engine.linked_transitions,
                (unsigned long long)engine.safepoint_returns,
                (unsigned long long)engine.reg_loads,
                (unsigned long long)engine.reg_stores,
                (unsigned long long)engine.reg_reconciliations,
                (unsigned long long)engine.reg_spills,
                (unsigned long long)engine.flags_safepoint_commits,
                window_count,
                counts.target_surfaces,presented,(unsigned long long)video.flips,
                (unsigned long long)audio_stats.blocks,
                (unsigned long long)audio_stats.input_bytes,
                (unsigned long long)audio_stats.output_frames,audio_stats.input_hash,
                (unsigned long long)audio_stats.enqueues,
                (unsigned long long)audio_completion_events,audio_stats.queue_depth,
                audio_stats.queue_high_water,(unsigned long long)audio_stats.queue_full,
                (unsigned long long)audio_stats.output_errors,
                (unsigned long long)pad.polls,(unsigned long long)pad.core.stats.samples,
                (unsigned long long)pad.core.stats.events,(unsigned long long)pad.connected_samples,
                (unsigned long long)pad.intercepted_samples,(unsigned long long)pad.read_errors,
                (unsigned long long)services.profile_lookups,
                (unsigned long long)services.profile_missing,
                (unsigned long long)services.profile_errors,
                (unsigned long long)services.profile_bytes,
                (unsigned long long)runtime.mci_calls,runtime.mci_last_command,
                (unsigned long long)idle_yields,(unsigned long long)idle_yield_ns,
                (unsigned long long)loop_gap_max_ns,(unsigned long long)loop_gaps_16ms,
                (unsigned long long)loop_gaps_33ms);
            PS5LOG_LOG("PW_AUDIO_QUEUE schema=1 worker=%u enqueues=%llu completions=%llu "
                "depth=%u high_water=%u full=%llu blocks=%llu output_errors=%llu",
                audio_stats.worker_running,(unsigned long long)audio_stats.enqueues,
                (unsigned long long)audio_completion_events,audio_stats.queue_depth,
                audio_stats.queue_high_water,(unsigned long long)audio_stats.queue_full,
                (unsigned long long)audio_stats.blocks,
                (unsigned long long)audio_stats.output_errors);
            PS5LOG_LOG("PW_GDI_STRETCH schema=1 calls=%llu owner=0x%08x dst=%d,%d,%d,%d src=%d,%d,%d,%d dib=%ux%u bits=0x%08x info=0x%08x",
                (unsigned long long)gdi.stretch_calls,gdi.stretch_owner,
                gdi.stretch_x,gdi.stretch_y,gdi.stretch_width,gdi.stretch_height,
                gdi.stretch_source_x,gdi.stretch_source_y,gdi.stretch_source_width,
                gdi.stretch_source_height,gdi.stretch_dib_width,gdi.stretch_dib_height,
                gdi.stretch_bits,gdi.stretch_info);
            last_heartbeat=now;
        }
    }

    /* Normal shutdown is best-effort but exhaustive.  Preserve every result
     * in one record so a later launch can distinguish a clean guest exit from
     * a title-manager kill, which cannot run process cleanup code. */
    if(supervisor) {
        if((status=pw_runtime_supervisor_request_stop(supervisor))!=PW_OK ||
           (status=pw_runtime_supervisor_guest_exited(supervisor,
               (int32_t)exit_code))!=PW_OK)
            abort_runtime("runtime-session-exit",status);
    }
    const uint64_t final_retired=engine.retired_instructions,final_flips=video.flips;
    PwAudioPs5Stats final_audio_stats;
    if(pw_audio_ps5_stats(&audio,&final_audio_stats)!=PW_OK)
        abort_runtime("audio-final-stats",PW_ERR_STATE);
    const uint64_t final_audio_blocks=final_audio_stats.blocks;
    uint32_t final_state_written=0;int state_close=PW_OK;
    if(registry.generation!=saved_generation)
        state_close=pw_state_ps5_save_registry(&registry,registry_path,state_buffer,
            PW_STATE_PS5_MAX_BYTES,&final_state_written);
    PwGdiTargetView close_view;uint32_t close_window=presentation_window(&user32,&gdi,&close_view);
    int pad_close=pw_pad_ps5_close(&pad,&user32,close_window);
    int audio_close=pw_audio_ps5_close(&audio);
    int gdi_close=pw_gdi_reset(&gdi);
    int video_close=pw_videoout_ps5_close(&video);
    int dbt_close=pw_x86_engine_destroy(&engine);
    int image_close=pw_map_release(&mapped,&vm);
    provider.close(provider.context,&root);
    int stack_close=vm.release(vm.context,&stack);
    int thread_close=vm.release(vm.context,&thread);
    int crt_close=vm.release(vm.context,&crt);
    int heap_close=vm.release(vm.context,&heap_region);
    int cleanup_status=PW_OK;
    collect_cleanup_status(&cleanup_status,state_close);
    collect_cleanup_status(&cleanup_status,pad_close);
    collect_cleanup_status(&cleanup_status,audio_close);
    collect_cleanup_status(&cleanup_status,gdi_close);
    collect_cleanup_status(&cleanup_status,video_close);
    collect_cleanup_status(&cleanup_status,dbt_close);
    collect_cleanup_status(&cleanup_status,image_close);
    collect_cleanup_status(&cleanup_status,stack_close);
    collect_cleanup_status(&cleanup_status,thread_close);
    collect_cleanup_status(&cleanup_status,crt_close);
    collect_cleanup_status(&cleanup_status,heap_close);
    for(unsigned slot=0;slot<8;slot++)if(files->streams[slot]>=0) {
        int stream_close=pw_file_ps5_stream_close(files,0x0d000001u+slot);
        collect_cleanup_status(&cleanup_status,stream_close);
    }
    int supervisor_cleanup=PW_OK;
    if(supervisor) {
        supervisor_cleanup=pw_runtime_supervisor_cleanup_complete(
            supervisor,cleanup_status);
    }
    PS5LOG_LOG("PW_RUNTIME_TEARDOWN schema=2 reason=%s exit_code=%u state=%s state_bytes=%u "
        "pad=%s pad_close_rc=%d user_terminate_rc=%d audio=%s gdi=%s "
        "video=%s unregister_rc=0x%08x video_close_rc=0x%08x video_munmap_rc=0x%08x "
        "video_release_rc=0x%08x agc=%s agc_unmap_rc=0x%08x agc_release_rc=0x%08x "
        "agc_munmap_rc=0x%08x agc_unload_rc=0x%08x dbt=%s image=%s stack=%s thread=%s crt=%s heap=%s"
        " session=%u outcome=%u supervisor_state=%u supervisor_cleanup=%s",
        exit_reason,exit_code,pw_result_name(state_close),final_state_written,
        pw_result_name(pad_close),pad.close_rc,pad.terminate_rc,pw_result_name(audio_close),
        pw_result_name(gdi_close),pw_result_name(video_close),(uint32_t)video.unregister_rc,
        (uint32_t)video.close_rc,(uint32_t)video.munmap_rc,(uint32_t)video.release_rc,
        pw_result_name(video.agc_close_rc),(uint32_t)video.agc.unmap_rc,
        (uint32_t)video.agc.release_rc,(uint32_t)video.agc.munmap_rc,
        (uint32_t)video.agc.unload_rc,pw_result_name(dbt_close),pw_result_name(image_close),
        pw_result_name(stack_close),pw_result_name(thread_close),pw_result_name(crt_close),
        pw_result_name(heap_close),supervisor?supervisor->last_result.session_id:0u,
        supervisor?(unsigned)supervisor->last_result.outcome:0u,
        supervisor?(unsigned)supervisor->state:(unsigned)PW_RUNTIME_IDLE,
        pw_result_name(supervisor_cleanup));
    PS5LOG_LOG("PW_RUNTIME_END schema=1 reason=%s exit_code=%u events=%llu retired=%llu flips=%llu audio_blocks=%llu",
        exit_reason,exit_code,(unsigned long long)events,
        (unsigned long long)final_retired,(unsigned long long)final_flips,
        (unsigned long long)final_audio_blocks);
    (void)munmap(workspace,sizeof(*workspace));
    (void)munmap(audio_queue,PW_AUDIO_PS5_QUEUE_BLOCKS*sizeof(*audio_queue));
    (void)munmap(cache,8192u*sizeof(*cache));
    (void)munmap(pixels,32u*1024u*1024u);
    (void)munmap(surfaces,128u*sizeof(*surfaces));(void)munmap(dcs,128u*sizeof(*dcs));
    (void)munmap(classes,128u*sizeof(*classes));(void)munmap(resources,128u*sizeof(*resources));
    (void)munmap(windows,128u*sizeof(*windows));(void)munmap(messages,128u*sizeof(*messages));
    (void)munmap(state_buffer,PW_STATE_PS5_MAX_BYTES);
    (void)munmap(profile_buffer,64u*1024u);
    (void)munmap(registry_values,128u*sizeof(*registry_values));
    (void)munmap(registry_keys,32u*sizeof(*registry_keys));
    (void)munmap(heap_blocks,131072u*sizeof(*heap_blocks));(void)munmap(files,sizeof(*files));
    if(supervisor)(void)munmap(supervisor,sizeof(*supervisor));
    if(prefix_layout)(void)munmap(prefix_layout,sizeof(*prefix_layout));
    ps5log_close(exit_reason);return (int)exit_code;
}
