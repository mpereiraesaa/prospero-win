/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../src/pw_win32.h"
#include "../src/pw_vm_posix.h"
#include <assert.h>
#include <string.h>

#if defined(__clang__)
__attribute__((no_sanitize("function")))
#endif
static int invoke(void *code,PwX86State *state){return ((int (*)(PwX86State *))code)(state);}
static int valid_code(void *opaque,uint32_t address)
{(void)opaque;return address==0x01002000?PW_OK:PW_ERR_NOT_FOUND;}

static void callback_code(PwVmBackend *vm,PwVmRegion *code,uint32_t answer)
{
    assert(vm->protect(NULL,code,0,code->bytes,PW_PROT_READ|PW_PROT_WRITE)==PW_OK);
    uint8_t source[]={0xb8,(uint8_t)answer,(uint8_t)(answer>>8),(uint8_t)(answer>>16),
        (uint8_t)(answer>>24),0xc2,16,0};PwX86Block block;
    assert(pw_x86_translate(source,sizeof(source),0x01002000,code->write_base,code->bytes,&block)==PW_OK);
    assert(vm->protect(NULL,code,0,code->bytes,PW_PROT_READ|PW_PROT_EXEC)==PW_OK);
}
static void enter_create(PwWin32 *runtime,PwX86State *state,uint32_t token,
                         uint32_t class_name,uint32_t title)
{
    uint32_t frame[]={0x01003000,0,class_name,title,0x80000000u,(uint32_t)-10,
        (uint32_t)-10,1,1,0,0,0x01000000,0};
    state->eip=token;state->gpr[4]=state->stack_high-sizeof(frame);
    memcpy((void *)(uintptr_t)state->gpr[4],frame,sizeof(frame));
    assert(pw_win32_dispatch(runtime,state)==PW_OK && runtime->callback_pending &&
           runtime->create[0].active && runtime->create_depth==1 && state->eip==0x01002000);
}
int main(void)
{
    PwVmBackend vm;PwVmRegion memory,code;
    assert(pw_vm_posix_backend(&vm)==PW_OK);
    assert(vm.reserve_at(NULL,0x03000000,8192,4096,&memory)==PW_OK);
    assert(vm.commit(NULL,&memory,0,memory.bytes,PW_PROT_READ|PW_PROT_WRITE)==PW_OK);
    assert(vm.reserve(NULL,4096,4096,&code)==PW_OK);
    assert(vm.commit(NULL,&code,0,code.bytes,PW_PROT_READ|PW_PROT_WRITE)==PW_OK);
    PwWin32 runtime;assert(pw_win32_init(&runtime,0x01000000,0x03000000,"test")==PW_OK);
    runtime.services.code_address=valid_code;
    PwUser32 user;PwUser32Message messages[2];PwUser32Window windows[2];
    PwUser32Resource resources[2];PwUser32Class classes[2];
    assert(pw_user32_init(&user,messages,2,windows,2)==PW_OK);
    assert(pw_user32_init_resources(&user,resources,2)==PW_OK);
    assert(pw_user32_configure_desktop(&user,1920,1080)==PW_OK);
    assert(pw_user32_init_classes(&user,classes,2)==PW_OK);runtime.user32=&user;
    PwUser32Class descriptor={.wndproc=0x01002000,.module=0x01000000,.window_extra=4};
    strcpy(descriptor.class_name,"Splash");uint16_t atom;
    assert(pw_user32_register_class(&user,&descriptor,&atom)==PW_OK);
    uint32_t class_name=0x03000100,title=0x03000120;
    strcpy((char *)(uintptr_t)class_name,"Splash");*(char *)(uintptr_t)title=0;
    PeImportSymbol symbol={0};strcpy(symbol.name,"CreateWindowExA");PwImportTarget target;
    assert(pw_win32_resolve(&runtime,"user32.dll",&symbol,&target)==PW_OK);
    uint32_t create_token=(uint32_t)target.address;
    PwX86State state={.stack_low=0x03001000,.stack_high=0x03002000,.memory_count=1};
    state.memory[0]=(PwX86Memory){0x03000000,0x03001000,PW_X86_READ|PW_X86_WRITE};

    callback_code(&vm,&code,1);enter_create(&runtime,&state,create_token,class_name,title);
    uint32_t callback_args[5];memcpy(callback_args,(void *)(uintptr_t)state.gpr[4],sizeof(callback_args));
    assert(callback_args[0]==PW_WIN32_WINDOW_CALLBACK && callback_args[2]==0x81 && callback_args[4]);
    uint32_t create[12];memcpy(create,(void *)(uintptr_t)callback_args[4],sizeof(create));
    assert(create[1]==0x01000000 && create[4]==1 && create[5]==1 &&
           create[8]==0x80000000u && create[9]==title && create[10]==class_name);
    assert(invoke(code.exec_base,&state)==0 && state.eip==PW_WIN32_WINDOW_CALLBACK);
    assert(pw_win32_dispatch(&runtime,&state)==PW_OK && runtime.callback_pending &&
           runtime.create[0].phase==2 && state.eip==0x01002000);
    assert(invoke(code.exec_base,&state)==0 && state.eip==PW_WIN32_WINDOW_CALLBACK);
    assert(pw_win32_dispatch(&runtime,&state)==PW_OK && !runtime.create[0].active && !runtime.create_depth &&
           state.eip==0x01003000 && state.gpr[4]==state.stack_high && state.gpr[0]==0x10000 &&
           windows[0].used && !windows[0].creating && runtime.calls==1);

    strcpy(symbol.name,"SetWindowLongA");
    assert(pw_win32_resolve(&runtime,"user32.dll",&symbol,&target)==PW_OK);
    state.eip=(uint32_t)target.address;state.gpr[4]=state.stack_high-16;
    uint32_t set_frame[]={0x01003000,0x10000,0,0x03000180};
    memcpy((void *)(uintptr_t)state.gpr[4],set_frame,sizeof(set_frame));
    assert(pw_win32_dispatch(&runtime,&state)==PW_OK && !state.gpr[0] &&
           state.eip==set_frame[0] && state.gpr[4]==state.stack_high && runtime.calls==2);
    uint32_t stored=0;memcpy(&stored,windows[0].extra,sizeof(stored));assert(stored==set_frame[3]);

    strcpy(symbol.name,"GetDesktopWindow");
    assert(pw_win32_resolve(&runtime,"user32.dll",&symbol,&target)==PW_OK);
    state.eip=(uint32_t)target.address;state.gpr[4]=state.stack_high-4;
    memcpy((void *)(uintptr_t)state.gpr[4],set_frame,4);
    assert(pw_win32_dispatch(&runtime,&state)==PW_OK &&
           state.gpr[0]==PW_USER32_DESKTOP_HANDLE && runtime.calls==3);
    strcpy(symbol.name,"GetWindowRect");
    assert(pw_win32_resolve(&runtime,"user32.dll",&symbol,&target)==PW_OK);
    uint32_t rect_address=0x030001a0;
    uint32_t rect_frame[]={0x01003000,PW_USER32_DESKTOP_HANDLE,rect_address};
    state.eip=(uint32_t)target.address;state.gpr[4]=state.stack_high-sizeof(rect_frame);
    memcpy((void *)(uintptr_t)state.gpr[4],rect_frame,sizeof(rect_frame));
    assert(pw_win32_dispatch(&runtime,&state)==PW_OK && state.gpr[0]==1 && runtime.calls==4);
    PwUser32Rect rect;memcpy(&rect,(void *)(uintptr_t)rect_address,sizeof(rect));
    assert(rect.left==0 && rect.top==0 && rect.right==1920 && rect.bottom==1080);

    strcpy(symbol.name,"SetWindowLongA");
    assert(pw_win32_resolve(&runtime,"user32.dll",&symbol,&target)==PW_OK);
    state.eip=(uint32_t)target.address;state.gpr[4]=state.stack_high-16;
    set_frame[2]=1;set_frame[3]=0;memcpy((void *)(uintptr_t)state.gpr[4],set_frame,sizeof(set_frame));
    assert(pw_win32_dispatch(&runtime,&state)==PW_OK && !state.gpr[0] &&
           runtime.last_error==1413 && stored==0x03000180 && runtime.calls==5);
    memcpy(&stored,windows[0].extra,sizeof(stored));assert(stored==0x03000180);

    callback_code(&vm,&code,0);enter_create(&runtime,&state,create_token,class_name,title);
    assert(invoke(code.exec_base,&state)==0 && state.eip==PW_WIN32_WINDOW_CALLBACK);
    assert(pw_win32_dispatch(&runtime,&state)==PW_OK && !runtime.create[0].active && !runtime.create_depth &&
           !state.gpr[0] && !windows[1].used && user.next_object==0x10001 && runtime.calls==6);

    /* WM_QUIT is a thread message: DispatchMessageA has no window procedure
     * to run and returns 0 without entering a callback. */
    strcpy(symbol.name,"DispatchMessageA");
    assert(pw_win32_resolve(&runtime,"user32.dll",&symbol,&target)==PW_OK);
    const PwUser32QueueEntry quit={.window=0,.message=0x0012};
    memcpy((void *)(uintptr_t)0x03000200,&quit,sizeof(quit));
    uint32_t dispatch_frame[]={0x01003000,0x03000200};
    state.eip=(uint32_t)target.address;state.gpr[4]=state.stack_high-sizeof(dispatch_frame);
    state.gpr[0]=0xdeadbeefu;
    memcpy((void *)(uintptr_t)state.gpr[4],dispatch_frame,sizeof(dispatch_frame));
    assert(pw_win32_dispatch(&runtime,&state)==PW_OK && !state.gpr[0] &&
           state.eip==0x01003000 && state.gpr[4]==state.stack_high &&
           !runtime.callback_pending && !runtime.message_dispatch.active);
    /* A message for a window that does not exist is still refused. */
    const PwUser32QueueEntry stale={.window=0x10077,.message=0x000f};
    memcpy((void *)(uintptr_t)0x03000200,&stale,sizeof(stale));
    state.eip=(uint32_t)target.address;state.gpr[4]=state.stack_high-sizeof(dispatch_frame);
    memcpy((void *)(uintptr_t)state.gpr[4],dispatch_frame,sizeof(dispatch_frame));
    assert(pw_win32_dispatch(&runtime,&state)!=PW_OK && !runtime.message_dispatch.active);
    assert(vm.release(NULL,&code)==PW_OK);assert(vm.release(NULL,&memory)==PW_OK);return 0;
}
