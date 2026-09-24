/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_WIN32_H
#define PW_WIN32_H
#include "pw_import_bind.h"
#include "pw_guest_call.h"
#include "pw_guest_args.h"
#include "pw_guest_heap.h"
#include "pw_registry.h"
#include "pw_user32.h"
#include "pw_gdi.h"
enum { PW_WIN32_TOKEN_BASE=0xe0000000u, PW_WIN32_CALLBACK_BASE=0xe1000000u,
       PW_WIN32_INIT_DEPTH=8, PW_WIN32_INIT_ENTRIES=1024,
       PW_WIN32_CREATE_DEPTH=8,
       PW_WIN32_CREATE_CALLBACK_BASE=PW_WIN32_CALLBACK_BASE+PW_WIN32_INIT_DEPTH*16,
       PW_WIN32_WINDOW_CALLBACK=PW_WIN32_CREATE_CALLBACK_BASE,
       PW_WIN32_UPDATE_CALLBACK=PW_WIN32_CREATE_CALLBACK_BASE+PW_WIN32_CREATE_DEPTH*16,
       PW_WIN32_DISPATCH_CALLBACK=PW_WIN32_UPDATE_CALLBACK+16 };
typedef struct PwWin32Init {
    PwGuestCall call;
    PwGuestCallback callback;
    uint32_t cursor,end;
} PwWin32Init;
typedef struct PwWin32Create {
    PwGuestCall call;
    PwGuestCallback callback;
    uint32_t slot,handle,wndproc,create_struct,phase;
    unsigned active;
} PwWin32Create;
typedef struct PwWin32Update {
    PwGuestCall call;
    PwGuestCallback callback;
    uint32_t handle,wndproc;
    unsigned active;
} PwWin32Update;
typedef struct PwWin32MessageDispatch {
    PwGuestCall call;
    PwGuestCallback callback;
    uint32_t handle,wndproc;
    unsigned active;
} PwWin32MessageDispatch;
typedef struct PwWaveOut {
    uint32_t handle,callback,callback_instance,callback_flags;
    uint32_t samples_per_second,average_bytes_per_second;
    uint16_t channels,block_align,bits_per_sample;
    unsigned open,paused;
    uint64_t bytes_submitted,bytes_completed;
    uint32_t headers[32];
    unsigned header_count;
} PwWaveOut;
typedef struct PwResourceRef {
    uint32_t handle,size;unsigned used;
} PwResourceRef;
typedef struct PwMmio {
    uint32_t handle,file;unsigned open;
} PwMmio;
typedef enum PwClockDomain { PW_CLOCK_UTC=1, PW_CLOCK_UPTIME=2, PW_CLOCK_COUNTER=3 } PwClockDomain;
typedef enum PwAudioControl { PW_AUDIO_PAUSE=1,PW_AUDIO_RESTART=2,PW_AUDIO_RESET=3,
                              PW_AUDIO_CLOSE=4 } PwAudioControl;
typedef struct PwWin32Services {
    void *opaque;
    /* UTC: nanoseconds since Unix epoch (nonnegative); uptime: ns since boot
     * including suspend; counter: monotonic nanoseconds, frequency 1 GHz.
     * Return PW_OK only when the selected domain was actually sampled. */
    int (*clock_ns)(void *,PwClockDomain,uint64_t *);
    uint32_t process_id,thread_id; /* guest registry IDs, not host handles */
    /* Borrowed UTF-16LE from a live module's resources. NOT_FOUND is absence,
     * not malformed input. Provider owns module and language selection. */
    int (*string_resource)(void *,uint32_t,uint32_t,const uint8_t **,size_t *);
    /* Borrowed bytes for an exact named PE resource. The provider validates
     * the module and owns language selection and byte lifetime. */
    int (*named_resource)(void *,uint32_t,uint32_t,const char *,const uint8_t **,size_t *);
    int (*integer_resource)(void *,uint32_t,uint32_t,uint32_t,const uint8_t **,size_t *);
    /* Confirms that a guest callback begins in executable image memory. */
    int (*code_address)(void *,uint32_t);
    unsigned ansi_codepage; /* currently exact CP1252 conversion only */
    /* Stable guest-visible DOS path for the main image, never a host path. */
    const char *main_module_filename;
    /* Borrowed absolute DOS path returned by GetCurrentDirectoryA. */
    const char *current_directory;
    /* CRT stream handles are provider-owned opaque 32-bit tokens. Paths are
     * guest DOS paths; providers must confine translation to the title root. */
    int (*file_open)(void *,const char *,const char *,uint32_t *);
    int (*file_close)(void *,uint32_t);
    int (*file_read)(void *,uint32_t,void *,uint32_t,uint32_t *);
    int (*file_seek)(void *,uint32_t,int32_t,uint32_t,uint32_t *);
    /* Provider-confined INI lookup used by GetPrivateProfileIntA. The
     * provider owns DOS-path translation and returns the default for a
     * missing file, section, key, or nonnumeric value. */
    int (*profile_int)(void *,const char *,const char *,uint32_t,const char *,uint32_t *);
    /* Blocking GetMessage adapter. The core checks the returned window and
     * serializes the provider-neutral Win32 MSG itself. */
    int (*message_wait)(void *,uint32_t filter_window,PwUser32QueueEntry *);
    /* Host scheduling adapter for kernel32 Sleep. */
    int (*sleep_ms)(void *,uint32_t milliseconds);
    int (*audio_open)(void *,uint32_t rate,uint16_t channels,uint16_t bits_per_sample);
    /* Submit copies PCM before returning. A nonzero token is returned exactly
     * once by audio_poll after the host has consumed the final sample. */
    int (*audio_submit)(void *,const void *pcm,uint32_t bytes,uint32_t token);
    int (*audio_poll)(void *,uint32_t *token,uint32_t *bytes);
    int (*audio_control)(void *,PwAudioControl);
} PwWin32Services;
typedef struct PwWin32 {
    uint32_t main_base,crt_data,app_type;
    uint32_t mci_last_command;
    uint64_t mci_calls;
    PwGuestArgs args;
    uint32_t new_mode;
    int32_t thread_priority; /* logical Win32 priority for the single guest thread */
    uint32_t rand_state; /* MSVCRT-compatible per-process generator state */
    PwGuestHeap *heap; /* owner-supplied arena, registered RW in guest memory */
    PwRegistry *registry; /* owner-supplied fixed-capacity Win32 registry */
    PwUser32 *user32; /* owner-supplied registered-message/window namespace */
    PwGdi *gdi; /* owner-supplied fixed-capacity GDI object/display namespace */
    PwWaveOut wave_out; /* guest-thread-owned WinMM PCM state */
    PwResourceRef resources[128]; /* process-lifetime copies in the guest heap */
    PwMmio mmio[8];
    uint32_t crt_errno; /* logical per-guest-thread errno; pointer export pending */
    uint32_t last_error; /* Win32 per-guest-thread error, distinct from CRT errno */
    uint16_t startup_show; /* explicit GUI launch profile: SW_SHOWNORMAL by default */
    PwWin32Services services;
    const char *last_dll,*last_name;
    unsigned calls;
    /* One-dispatch scheduling hint: the guest completed a nonblocking
     * PeekMessage with no matching work.  Providers may yield after the call;
     * the core never sleeps or changes guest-visible time itself. */
    unsigned idle_hint;
    uint32_t exit_code;
    unsigned exit_requested;
    unsigned callback_pending,init_depth;
    PwWin32Init init[PW_WIN32_INIT_DEPTH];
    PwWin32Create create[PW_WIN32_CREATE_DEPTH];
    unsigned create_depth;
    PwWin32Update update;
    PwWin32MessageDispatch message_dispatch;
} PwWin32;
/* Caller supplies a live RW, identity-mapped CRT region of at least 4096
 * bytes. Tokens occupy [TOKEN_BASE,TOKEN_BASE+catalog_count*16); the owner
 * must keep that range and [CALLBACK_BASE,CALLBACK_BASE+INIT_DEPTH*16)
 * separate from guest image/data/stack mappings. Runtime is guest-thread-owned.
 * Initialization packs command line, argv and an explicitly empty environment
 * into this region; combined storage must fit. Failure leaves it unchanged. */
int pw_win32_init(PwWin32 *runtime,uint32_t main_base,uint32_t crt_data,
                  const char *guest_command_line);
int pw_win32_resolve(void *,const char *,const PeImportSymbol *,PwImportTarget *);
/* PW_ERR_NOT_FOUND: not a token; UNSUPPORTED: named API/argument not covered.
 * No pending API reports success. Dispatcher must intercept tokens before
 * code fetch. Initial surface: GetModuleHandleA(NULL), __set_app_type,
 * __p__fmode, __p__commode and _controlfp (requires initialized state.fp).
 * __getmainargs supports non-wildcard argv/env and new_mode 0/1. The imported
 * Advapi registry group dispatches through owner-supplied PwRegistry storage.
 * _initterm can schedule guest callbacks: PW_OK with callback_pending=1 is
 * a yield, not an API return; fetch guest EIP next. Intercept callback tokens
 * here before fetching code. CRT pointer results refer to live guest words. */
int pw_win32_dispatch(PwWin32 *,PwX86State *);
/* Drain host audio completions on the guest thread. This is the only path
 * allowed to mutate WAVEHDR flags or post WOM_DONE into the Win32 queue. */
int pw_win32_pump_audio(PwWin32 *,PwX86State *,uint32_t *completed);
#endif
