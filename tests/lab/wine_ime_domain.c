/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Mixed-domain "Wine IME" UI class proof. Build once as a PE32 client and
 * once as a PE64 service DLL. The guest loads its imm32 IME first, then a
 * native service thread loads the native imm32 IME and resolves/creates the
 * same class with its own imm32 instance, as imm32's get_ime_ui_window does. */
#include <windows.h>
#include <imm.h>
#include <stdint.h>
#include <stdio.h>
#define CHECK_PROC(proc,module) ((uintptr_t)(proc)>=(uintptr_t)(module) && \
    (uintptr_t)(proc)<(uintptr_t)(module)+((IMAGE_NT_HEADERS *)((char *)(module)+((IMAGE_DOS_HEADER *)(module))->e_lfanew))->OptionalHeader.SizeOfImage)
static LONG proc_calls;
static WNDPROC original;
/* Counts messages that reach this domain's own imm32 procedure. */
static LRESULT CALLBACK counting_proc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp)
{InterlockedIncrement(&proc_calls);return CallWindowProcW(original,hwnd,msg,wp,lp);}
/* Loads this domain's IME (ImeInquire registers "Wine IME"), then checks the
 * class resolved for this domain's imm32 instance. Bit 0: IME loaded; bit 1:
 * class procedure lies inside this domain's imm32; bit 2: a "Wine IME" window
 * was created with this instance, its procedure is ours, and it was destroyed;
 * bit 3: CreateWindowExW returned a window (its procedure replaces *proc_out). */
static uint64_t probe(int create,uint64_t *proc_out,uint64_t *module_out)
{
    HMODULE imm=LoadLibraryW(L"imm32.dll");WNDCLASSEXW wc={.cbSize=sizeof(wc)};uint64_t flags=0;
    if(!imm)return 0;
    *module_out=(uintptr_t)imm;
    if(ImmGetProperty(GetKeyboardLayout(0),IGP_PROPERTY))flags|=1;
    if(GetClassInfoExW(imm,L"Wine IME",&wc)){*proc_out=(uintptr_t)wc.lpfnWndProc;if(CHECK_PROC(wc.lpfnWndProc,imm))flags|=2;}
    if(create==2||(create&&(flags&2))){ /* 2 forces creation to reproduce the fault */
        HWND hwnd=CreateWindowExW(WS_EX_TOOLWINDOW,L"Wine IME",NULL,WS_POPUP,0,0,1,1,NULL,NULL,imm,NULL);
        if(hwnd){
            WNDPROC proc=(WNDPROC)GetWindowLongPtrW(hwnd,GWLP_WNDPROC);
            flags|=8;*proc_out=(uintptr_t)proc;
            if(CHECK_PROC(proc,imm)){
                original=proc;SetWindowLongPtrW(hwnd,GWLP_WNDPROC,(LONG_PTR)counting_proc);
                SendMessageW(hwnd,WM_NULL,0,0);
                SetWindowLongPtrW(hwnd,GWLP_WNDPROC,(LONG_PTR)original);
                if(proc_calls>0)flags|=4;
            }
            DestroyWindow(hwnd);
        }
    }
    return flags;
}
#ifdef _WIN64
static int native_teb(void) { return *(volatile LONG *)((char *)NtCurrentTeb() + 0x180c) == 0; }
__declspec(dllexport) DWORD WINAPI PwD3D9ServiceMain(uint64_t *result)
{
    /* The broker zeroes results and the WoW64 guest environment block is not
     * the native one, so the guest names the mode with an event. */
    HANDLE create=OpenEventW(SYNCHRONIZE,FALSE,L"Local\\PW_WINE_IME_CREATE"),force=OpenEventW(SYNCHRONIZE,FALSE,L"Local\\PW_WINE_IME_FORCE");
    if(!native_teb())return 10;
    result[3]=probe(force?2:create?1:0,&result[4],&result[5]);
    if(create)CloseHandle(create);
    if(force)CloseHandle(force);
    return 0;
}
#else
struct request { ULONG version,size; WCHAR path[260]; uint64_t result[8]; };
typedef LONG (WINAPI *query_fn)(HANDLE,ULONG,void *,ULONG,ULONG *);
int main(int argc,char **argv)
{
    struct request req={.version=1,.size=sizeof(struct request)};ULONG size=0;
    query_fn query=(query_fn)GetProcAddress(GetModuleHandleA("ntdll.dll"),"NtQueryInformationProcess");
    uint64_t guest_proc=0,guest_module=0,guest=0,after=0,after_proc=0,after_module=0;
    if(argc!=3||!query)return 1;
    guest=probe(0,&guest_proc,&guest_module);
    MultiByteToWideChar(CP_UTF8,0,argv[1],-1,req.path,260);
    HANDLE mode=!strcmp(argv[2],"force")?CreateEventW(NULL,TRUE,FALSE,L"Local\\PW_WINE_IME_FORCE"):
                !strcmp(argv[2],"create")?CreateEventW(NULL,TRUE,FALSE,L"Local\\PW_WINE_IME_CREATE"):NULL;
    LONG status=query(GetCurrentProcess(),0x50570001,&req,sizeof(req),&size);
    if(mode)CloseHandle(mode);
    /* The guest's own class still resolves to the guest procedure afterwards. */
    after=probe(1,&after_proc,&after_module);
    printf("PW_WINE_IME mode=%s status=%08lx guest=%llu guest_proc=%llx guest_imm32=%llx native=%llu native_proc=%llx native_imm32=%llx guest_after=%llu\n",
           argv[2],status,(unsigned long long)guest,(unsigned long long)guest_proc,(unsigned long long)guest_module,
           (unsigned long long)req.result[3],(unsigned long long)req.result[4],(unsigned long long)req.result[5],(unsigned long long)after);
    fflush(stdout);
    return status||guest!=3||after!=15||req.result[3]!=(strcmp(argv[2],"classinfo")?15u:3u);
}
#endif
