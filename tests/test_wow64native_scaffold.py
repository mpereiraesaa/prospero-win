#!/usr/bin/env python3
"""Inspect and exercise built scaffold artifacts; never enter compatibility mode."""
import argparse,ctypes,json,subprocess,threading,re,os,sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
p=argparse.ArgumentParser()
p.add_argument('artifacts',type=Path,nargs='?',default=os.environ.get('PROSPERO_WOW64NATIVE_ARTIFACTS'),help='built native backend artifacts; or PROSPERO_WOW64NATIVE_ARTIFACTS')
p.add_argument('--wine-build',type=Path,help='also execute the PE ABI fixture using this pinned host Wine build')
p.add_argument('--wine-source',type=Path,help='Wine headers for the platform-gate fixture (default: sibling source directory)')
a=p.parse_args()
if a.artifacts is None:
    print('SKIP wow64native artifact checks: supply built artifacts as a positional argument or PROSPERO_WOW64NATIVE_ARTIFACTS')
    sys.exit(0)
class Init(ctypes.Structure):
    _fields_=[('version',ctypes.c_uint32),('ready',ctypes.c_uint32),('fs',ctypes.c_uint32),('reserved',ctypes.c_uint32),('fs_set_proc',ctypes.c_uint64)]
assert ctypes.sizeof(Init)==24
module=ctypes.CDLL(str((a.artifacts/'x86_64-unix/wow64native.so').resolve()))
entries=(ctypes.c_void_p*4).in_dll(module,'__wine_unix_call_funcs')
entry=entries[0]
init=ctypes.CFUNCTYPE(ctypes.c_uint32,ctypes.c_void_p)(entry)
assert init(None)==0xc000000d
for version in [0,1,2,0xffffffff]:
    params=Init(version,0,0,0)
    assert init(ctypes.byref(params))==0xc000000d
for ready in [0,1,0xffffffff]:
    params=Init(3,ready,0xffff,0)
    assert init(ctypes.byref(params))==0xc00000bb
    assert params.ready==0 and params.fs==0

class ThreadParams(ctypes.Structure):
    _fields_=[('version',ctypes.c_uint32),('reserved',ctypes.c_uint32),
              ('guest_teb',ctypes.c_uint64),('state',ctypes.c_uint64)]
class ThreadState(ctypes.Structure):
    _fields_=[('host_fs',ctypes.c_uint64),('guest_fs',ctypes.c_uint64),
              ('status',ctypes.c_uint32),('reserved',ctypes.c_uint32),('profile',ctypes.c_uint64)]
assert ctypes.sizeof(ThreadParams)==24 and ctypes.sizeof(ThreadState)==32
thread_init,thread_get,thread_term=[ctypes.CFUNCTYPE(ctypes.c_uint32,ctypes.c_void_p)(p) for p in entries[1:]]
for fn in [thread_init,thread_get]:
    assert fn(None)==0xc000000d
    assert fn(ctypes.byref(ThreadParams(2,0,0x20000,0)))==0xc000000d
for address in [0,0xffff,0xfffff001,0x100000000]:
    params=ThreadParams(3,0,address,0xffff)
    assert thread_init(ctypes.byref(params))==0xc0000141
    assert params.state==0

# Both workers must be alive simultaneously: prove the built module's TLS
# isolates host/guest state, and cleanup does not retain a previous TEB.
barrier=threading.Barrier(2)
def exercise_thread(address):
    params=ThreadParams(3,0,address,0)
    assert thread_get(ctypes.byref(params))==0xc000000d
    assert thread_init(ctypes.byref(params))==0xc00000bb
    first=params.state
    state=ThreadState.from_address(first)
    assert state.host_fs and state.guest_fs==address and state.status==0xc00000bb and not state.profile
    host_fs=state.host_fs
    barrier.wait(timeout=10)
    assert state.guest_fs==address
    assert thread_init(ctypes.byref(params))==0xc00000bb and params.state==first
    wrong=ThreadParams(3,0,address+0x1000,0xffff)
    assert thread_init(ctypes.byref(wrong))==0xc000000d and wrong.state==0
    assert thread_get(ctypes.byref(wrong))==0xc000000d and wrong.state==0
    params.state=0
    assert thread_get(ctypes.byref(params))==0xc00000bb and params.state==first
    assert thread_term(None)==0
    assert state.host_fs==state.guest_fs==state.status==0
    assert thread_get(ctypes.byref(params))==0xc000000d and params.state==0
    params.guest_teb+=0x1000
    assert thread_init(ctypes.byref(params))==0xc00000bb
    assert ThreadState.from_address(params.state).guest_fs==address+0x1000
    assert thread_term(None)==0
    return first,host_fs
with ThreadPoolExecutor(max_workers=2) as pool:
    results=list(pool.map(exercise_thread,[0x20000,0x40000]))
assert results[0][0]!=results[1][0] and results[0][1]!=results[1][1]
pe=a.artifacts/'x86_64-windows/wow64native.dll'
info=subprocess.check_output(['x86_64-w64-mingw32-objdump','-p',str(pe)],text=True)
for name in ['BTCpuGetBopCode','BTCpuGetContext','BTCpuIsProcessorFeaturePresent','BTCpuProcessInit','BTCpuResetToConsistentState','BTCpuSetContext','BTCpuSimulate','BTCpuThreadInit','BTCpuThreadTerm','BTCpuTurboThunkControl','__wine_get_unix_opcode']:
    assert name in info,name
assert '000000007a400000' in info.lower(),'low image base missing'
asm=subprocess.check_output(['x86_64-w64-mingw32-objdump','-dr',str(a.artifacts/'x86_64-windows/cpu.o')],text=True)
assert asm.count('ljmp')>=2 and 'iretq' in asm
assert asm.count('vextractf128')==32 and asm.count('vinsertf128')==32
assert 'pw_wow_run' not in asm and 'pw_x86_' not in asm
simulate=asm.split('<BTCpuSimulate>:',1)[1].split('\n\n',1)[0]
assert 'pw_native_prepare_simulate' in simulate,'entry guard call missing'
assert simulate.index('pw_native_prepare_simulate')<re.search(r'mov\s+%rax,%r15',simulate).start()
assert re.search(r'push\s+%r15',simulate),'state register must have an unwind save'
assert simulate.index('fxsave64')<simulate.index('pw_native_prepare_simulate')<simulate.index('fxrstor64')
assert len(re.findall(r'call[^\n]*<pw_native_restore_host_fs>',asm))==2
assert len(re.findall(r'call[^\n]*<pw_native_restore_guest_fs>',asm))==3
print(json.dumps({'init_cases':8,'thread_isolation_and_cleanup':True,'entry_guard_call_present':True,'low_PE_base':True,'native_transition_assembly_present':True,'no_DBT_run_import':True,'native_execution_tested':False}))

if a.wine_build:
    root=Path(__file__).resolve().parents[1]
    build=a.wine_build.resolve()
    source=(a.wine_source or build.parent/'source').resolve()
    out=(a.artifacts/'platform-gate-fixture').resolve()
    out.mkdir(parents=True,exist_ok=True)
    # Execute the actual console branch on the host, replacing only the
    # external readiness lookup and FS syscall. Never set a real FS base.
    (out/'mock.c').write_text(r'''
#include <stdint.h>
#include <string.h>
#include <dlfcn.h>
#include "wow64native.h"
static unsigned present, caps, fs_error;
unsigned last_version, get_calls, set_calls;
void mock_configure(unsigned p, unsigned c, unsigned e)
{ present=p; caps=c; fs_error=e; last_version=0; }
static unsigned query(unsigned version)
{ last_version=version; return version==PW_NW64_CAPS_VERSION ? caps : 0; }
int __wine_dbg_output(const char *str) { (void)str; return 0; }
void *dlsym(void *handle, const char *name)
{ return handle==RTLD_DEFAULT && present && !strcmp(name,PW_NATIVE_SIGNAL_QUERY) ? (void *)query : 0; }
int sysarch(int operation, void *value)
{
    if(operation!=128) { __sync_fetch_and_add(&set_calls,1); return -1; }
    __sync_fetch_and_add(&get_calls,1);
    if(fs_error==1) return -1;
    if(fs_error==2) return 0; /* A success result without a usable host base. */
    *(uint64_t *)value=(uintptr_t)__builtin_thread_pointer();
    return 0;
}
''')
    subprocess.run(['gcc','-m64','-O2','-g','-fPIC','-shared','-Wl,-Bsymbolic','-Wl,-z,defs',
        '-D__PROSPERO__','-D__WINESRC__','-DWINE_UNIX_LIB','-D_REENTRANT',
        '-I'+str(root/'wine/wow64native'),'-I'+str(build/'include'),'-I'+str(source/'include'),
        str(root/'wine/wow64native/unix.c'),str(out/'mock.c'),'-o',str(out/'gate.so')],check=True)
    gate=ctypes.CDLL(str(out/'gate.so'))
    gate.mock_configure.argtypes=[ctypes.c_uint]*3
    funcs=(ctypes.c_void_p*4).in_dll(gate,'__wine_unix_call_funcs')
    g_init,g_thread,g_get,g_term=[ctypes.CFUNCTYPE(ctypes.c_uint32,ctypes.c_void_p)(v) for v in funcs]
    for present,caps in [(0,7),*[(1,c) for c in range(7)]]:
        gate.mock_configure(present,caps,0)
        params=Init(3,0xffffffff,0xffff,0,0xffff)
        assert g_init(ctypes.byref(params))==0xc00000bb
        assert params.ready==params.fs==params.fs_set_proc==0
    gate.mock_configure(1,7,0)
    params=Init(3,0,0,0)
    assert g_init(ctypes.byref(params))==0 and params.ready==1 and params.fs==0
    assert params.fs_set_proc==ctypes.cast(gate.sysarch,ctypes.c_void_p).value
    assert ctypes.c_uint.in_dll(gate,'last_version').value==1
    tp=ThreadParams(3,0,0x50000,0)
    gate.mock_configure(1,7,1)
    assert g_thread(ctypes.byref(tp))==0xc0000001 and not tp.state
    gate.mock_configure(1,7,2)
    assert g_thread(ctypes.byref(tp))==0xc0000141 and not tp.state
    gate.mock_configure(1,7,0)
    assert g_thread(ctypes.byref(tp))==0
    state=ThreadState.from_address(tp.state)
    assert state.host_fs and state.guest_fs==tp.guest_teb and state.status==0
    gate.mock_configure(1,3,0)
    assert g_init(ctypes.byref(params))==0xc00000bb
    assert g_get(ctypes.byref(tp))==0xc00000bb and state.status==0xc00000bb
    gate.mock_configure(1,7,0)
    assert g_init(ctypes.byref(params))==0
    assert g_get(ctypes.byref(tp))==0 and state.status==0
    assert g_term(None)==0 and state.host_fs==state.guest_fs==0
    barrier=threading.Barrier(2)
    def ready_thread(address):
        params=ThreadParams(3,0,address,0)
        assert g_thread(ctypes.byref(params))==0
        state=ThreadState.from_address(params.state)
        captured=state.host_fs
        barrier.wait(timeout=10)
        assert state.guest_fs==address and state.status==0
        assert g_term(None)==0
        return params.state,captured
    with ThreadPoolExecutor(max_workers=2) as pool:
        ready=list(pool.map(ready_thread,[0x60000,0x70000]))
    assert ready[0][0]!=ready[1][0] and ready[0][1]!=ready[1][1]
    assert ctypes.c_uint.in_dll(gate,'set_calls').value==0
    assert g_init(None)==0xc000000d
    assert g_init(ctypes.byref(Init(1,0,0,0)))==0xc000000d
    old_profile_env=os.environ.pop('PW_NATIVE_PROFILE',None)
    for value,enabled in [('0',False),('1',True),('yes',False)]:
        os.environ['PW_NATIVE_PROFILE']=value
        assert g_init(ctypes.byref(params))==0
        tp=ThreadParams(3,0,0x80000,0)
        assert g_thread(ctypes.byref(tp))==0
        state=ThreadState.from_address(tp.state)
        assert bool(state.profile)==enabled
        if enabled:
            data=(ctypes.c_uint64*9).from_address(state.profile)
            assert data[8] and list(data[:8])==[0]*8
        assert g_term(None)==0 and state.profile==0
    if old_profile_env is None:os.environ.pop('PW_NATIVE_PROFILE',None)
    else:os.environ['PW_NATIVE_PROFILE']=old_profile_env
    assert g_init(ctypes.byref(params))==0
    receipt=dict(missing_and_partial_caps_refused=8,ready_thread_isolation=True,
        FS_capture_failure_refused=True,readiness_revocation_verified=True,
        actual_FS_changed=False,native_execution_tested=False)
    (out/'receipt.json').write_text(json.dumps(receipt,indent=2)+'\n')
    print(json.dumps(dict(platform_gate_fixture_passed=True,**receipt)))

WRAPPER_FIXTURE_C = r"""
#include <windows.h>
#include <cpuid.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

uint64_t last_op,last_ptr,last_base,callback_flags,before_flags,after_flags,after_rax,after_rcx,after_rdx;
unsigned char before_fp[512] __attribute__((aligned(16)));
unsigned char after_fp[512] __attribute__((aligned(16)));
unsigned char xmm_seed[16] __attribute__((aligned(16)))={1,3,5,7,9,11,13,15,2,4,6,8,10,12,14,16};
unsigned int different_mxcsr=0x3f80;
int check_avx;
unsigned char ymm_seed[512] __attribute__((aligned(32)));
unsigned char before_ymm[512] __attribute__((aligned(32)));
unsigned char after_ymm[512] __attribute__((aligned(32)));
unsigned char caller_fp[512] __attribute__((aligned(16)));
extern void fake_sysarch(void);
extern void invoke(void *,void *);
__asm__(
".text\n.global fake_sysarch\nfake_sysarch:\n"
"pushfq\npopq callback_flags(%rip)\n"
"movq %rdi,last_op(%rip)\nmovq %rsi,last_ptr(%rip)\nmovq (%rsi),%rax\nmovq %rax,last_base(%rip)\n"
"movq $0x7777,%rcx\nmovq $0x8888,%rdx\nmovq $0x9999,%rdi\nmovq $0xaaaa,%rsi\n"
"movq $0xbbbb,%r8\nmovq $0xcccc,%r9\nmovq $0xdddd,%r10\nmovq $0xeeee,%r11\n"
"pxor %xmm0,%xmm0\npxor %xmm1,%xmm1\npxor %xmm2,%xmm2\npxor %xmm3,%xmm3\n"
"pxor %xmm4,%xmm4\npxor %xmm5,%xmm5\npxor %xmm6,%xmm6\npxor %xmm7,%xmm7\n"
"pxor %xmm8,%xmm8\npxor %xmm9,%xmm9\npxor %xmm10,%xmm10\npxor %xmm11,%xmm11\n"
"pxor %xmm12,%xmm12\npxor %xmm13,%xmm13\npxor %xmm14,%xmm14\npxor %xmm15,%xmm15\n"
"cmpl $0,check_avx(%rip)\nje 1f\nvzeroall\n1:\n"
"fldz\nldmxcsr different_mxcsr(%rip)\nxorl %eax,%eax\nret\n"
".global invoke\ninvoke:\n"
"pushq %r15\npushq %rbx\nsubq $0x28,%rsp\n"
"movq %rcx,%rbx\nmovq %rdx,%r15\nfxsave64 caller_fp(%rip)\n"
"movq $0x1122,%rcx\nmovq $0x3344,%rdx\nmovq $0x5566,%rax\n"
"movdqa xmm_seed(%rip),%xmm0\nfld1\n"
"cmpl $0,check_avx(%rip)\nje 2f\n"
"vmovdqa ymm_seed+0(%rip),%ymm0\nvmovdqa %ymm0,before_ymm+0(%rip)\n"
"vmovdqa ymm_seed+32(%rip),%ymm1\nvmovdqa %ymm1,before_ymm+32(%rip)\n"
"vmovdqa ymm_seed+64(%rip),%ymm2\nvmovdqa %ymm2,before_ymm+64(%rip)\n"
"vmovdqa ymm_seed+96(%rip),%ymm3\nvmovdqa %ymm3,before_ymm+96(%rip)\n"
"vmovdqa ymm_seed+128(%rip),%ymm4\nvmovdqa %ymm4,before_ymm+128(%rip)\n"
"vmovdqa ymm_seed+160(%rip),%ymm5\nvmovdqa %ymm5,before_ymm+160(%rip)\n"
"vmovdqa ymm_seed+192(%rip),%ymm6\nvmovdqa %ymm6,before_ymm+192(%rip)\n"
"vmovdqa ymm_seed+224(%rip),%ymm7\nvmovdqa %ymm7,before_ymm+224(%rip)\n"
"vmovdqa ymm_seed+256(%rip),%ymm8\nvmovdqa %ymm8,before_ymm+256(%rip)\n"
"vmovdqa ymm_seed+288(%rip),%ymm9\nvmovdqa %ymm9,before_ymm+288(%rip)\n"
"vmovdqa ymm_seed+320(%rip),%ymm10\nvmovdqa %ymm10,before_ymm+320(%rip)\n"
"vmovdqa ymm_seed+352(%rip),%ymm11\nvmovdqa %ymm11,before_ymm+352(%rip)\n"
"vmovdqa ymm_seed+384(%rip),%ymm12\nvmovdqa %ymm12,before_ymm+384(%rip)\n"
"vmovdqa ymm_seed+416(%rip),%ymm13\nvmovdqa %ymm13,before_ymm+416(%rip)\n"
"vmovdqa ymm_seed+448(%rip),%ymm14\nvmovdqa %ymm14,before_ymm+448(%rip)\n"
"vmovdqa ymm_seed+480(%rip),%ymm15\nvmovdqa %ymm15,before_ymm+480(%rip)\n"
"2:\n"
"fxsave64 before_fp(%rip)\nstc\nstd\npushfq\npopq before_flags(%rip)\n"
"call *%rbx\n"
"pushfq\npopq after_flags(%rip)\nfxsave64 after_fp(%rip)\n"
"movq %rax,after_rax(%rip)\nmovq %rcx,after_rcx(%rip)\nmovq %rdx,after_rdx(%rip)\n"
"cmpl $0,check_avx(%rip)\nje 3f\n"
"vmovdqa %ymm0,after_ymm+0(%rip)\n"
"vmovdqa %ymm1,after_ymm+32(%rip)\n"
"vmovdqa %ymm2,after_ymm+64(%rip)\n"
"vmovdqa %ymm3,after_ymm+96(%rip)\n"
"vmovdqa %ymm4,after_ymm+128(%rip)\n"
"vmovdqa %ymm5,after_ymm+160(%rip)\n"
"vmovdqa %ymm6,after_ymm+192(%rip)\n"
"vmovdqa %ymm7,after_ymm+224(%rip)\n"
"vmovdqa %ymm8,after_ymm+256(%rip)\n"
"vmovdqa %ymm9,after_ymm+288(%rip)\n"
"vmovdqa %ymm10,after_ymm+320(%rip)\n"
"vmovdqa %ymm11,after_ymm+352(%rip)\n"
"vmovdqa %ymm12,after_ymm+384(%rip)\n"
"vmovdqa %ymm13,after_ymm+416(%rip)\n"
"vmovdqa %ymm14,after_ymm+448(%rip)\n"
"vmovdqa %ymm15,after_ymm+480(%rip)\n"
"3:\nfxrstor64 caller_fp(%rip)\ncld\n"
"addq $0x28,%rsp\npopq %rbx\npopq %r15\nret\n");

static int __attribute__((sysv_abi)) fail_first(int op, uint64_t *base)
{
    static int calls;
    printf("failure_callback call=%d op=%d base=%llx\n",++calls,op,*base);
    fflush(stdout);
    return calls==1 ? -1 : 0;
}

static uint64_t profile[9];
static int reported;
static void __attribute__((sysv_abi)) fake_profile_report(void *state)
{
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0":"=r"(flags));
    if ((flags & 0x400) || ((uint64_t *)state)[3] != (uintptr_t)profile) reported=-100;
    else reported++;
    __asm__ volatile("fldz; pxor %%xmm0,%%xmm0; pxor %%xmm15,%%xmm15":::"xmm0","xmm15","memory");
    if(check_avx) __asm__ volatile("vzeroall":::"memory");
}
int main(int argc, char **argv)
{
    uint64_t state[]={0x12345678,0x87654321,0,0};
    HMODULE dll=LoadLibraryW(L"wow64native.dll");
    void **setter;
    unsigned int *avx_flag, a, b, c, d, lo, hi, avx_available=0;
    const char *names[]={"pw_native_restore_host_fs","pw_native_restore_guest_fs"};
    if (!dll) {printf("load_failed=%lu\n",GetLastError()); return 10;}
    setter=(void *)GetProcAddress(dll,"pw_native_sysarch");
    if(!setter) return 11;
    if(argc>1 && !strcmp(argv[1],"fail"))
    {
        *setter=fail_first;
        invoke(GetProcAddress(dll,names[1]),state);
        puts("ERROR_guest_resumed_after_failed_FS_switch");
        return 60;
    }
    if(argc>1) {profile[8]=(uintptr_t)fake_profile_report;state[3]=(uintptr_t)profile;}
    avx_flag=(void *)GetProcAddress(dll,"pw_native_avx");
    if(!avx_flag) return 13;
    if(__get_cpuid(1,&a,&b,&c,&d) && (c & bit_AVX) && (c & bit_OSXSAVE))
    {
        __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
        avx_available=(lo & 6)==6;
    }
    for(int i=0;i<512;i++) ymm_seed[i]=(unsigned char)(i*3+i/32);
    *setter=fake_sysarch;
    for(check_avx=0;check_avx<=(int)avx_available;check_avx++)
    {
    *avx_flag=check_avx;
    for(int i=0;i<2;i++)
    {
        void *fn=GetProcAddress(dll,names[i]);
        if(!fn) return 12;
        invoke(fn,state);
        if(last_op!=129 || last_ptr!=(uintptr_t)&state[i] || last_base!=state[i]) return 20+i;
        if(after_rax!=0x5566 || after_rcx!=0x1122 || after_rdx!=0x3344) return 30+i;
        if(before_flags!=after_flags) return 40+i;
        if(callback_flags & 0x400) return 80+i;
        if(memcmp(before_fp,after_fp,512)) return 50+i;
        if(check_avx && memcmp(before_ymm,after_ymm,512)) return 70+i;
        printf("%s avx=%d: ABI, register, flags, x87/SSE/YMM preservation passed\n",names[i],check_avx);
    }
    }
    if(state[3]) {
        unsigned expected=avx_available?2:1;
        if(profile[0]!=expected || profile[1]!=expected || !profile[2] || !profile[3] || !profile[6] || reported!=1) return 90;
        printf("profile_on_preservation host=%llu guest=%llu reporter=%d\n",profile[0],profile[1],reported);
    }
    printf("AVX_cases=%u\n",avx_available?2:0);
    return 0;
}
"""

if a.wine_build:
    build = a.wine_build.resolve()
    out = (a.artifacts / 'abi-fixture').resolve()
    out.mkdir(exist_ok=True)
    root = Path(__file__).resolve().parents[1]
    (out / 'check.c').write_text(WRAPPER_FIXTURE_C)
    (out / 'wow64native.spec').write_text((root / 'wine/wow64native/wow64native.spec').read_text()
        + '\n@ stdcall pw_native_restore_host_fs()\n@ stdcall pw_native_restore_guest_fs()\n@ extern pw_native_sysarch\n@ extern pw_native_avx\n')
    subprocess.run([str(build / 'tools/winegcc/winegcc'), '-o', str(out / 'wow64native.dll'),
        '--wine-objdir', '.', '--cc-cmd=x86_64-w64-mingw32-gcc', '-b', 'x86_64-w64-mingw32',
        '-Wl,--wine-builtin', '-shared', str(out / 'wow64native.spec'), '-nodefaultlibs',
        '-Wl,--image-base,0x7a400000', str((a.artifacts / 'x86_64-windows/cpu.o').resolve()),
        'dlls/wow64/x86_64-windows/libwow64.a', 'dlls/ntdll/x86_64-windows/libntdll.a',
        'libs/winecrt0/x86_64-windows/libwinecrt0.a', 'dlls/ntdll/x86_64-windows/libntdll.a',
        'libs/compiler-rt/x86_64-windows/libcompiler-rt.a'], cwd=build, check=True)
    subprocess.run(['x86_64-w64-mingw32-gcc', '-O2', str(out / 'check.c'), '-o', str(out / 'check.exe')], check=True)
    unix = out / 'wow64native.so'
    if not unix.exists():
        unix.symlink_to((a.artifacts / 'x86_64-unix/wow64native.so').resolve())
    env = os.environ.copy()
    env.update(WINEPREFIX=str(out / 'prefix'), WINEARCH='win64', WINEDEBUG='-all',
        WINEDLLPATH=str(out), WINEDLLOVERRIDES='winemenubuilder.exe,winebus=d')
    cases = []
    for args, expected in [([], 0), (['profile'], 0), (['fail'], 1)]:
        run = subprocess.run([str(build / 'loader/wine'), str(out / 'check.exe'), *args],
            env=env, capture_output=True, text=True, timeout=45)
        record = dict(args=args, exit=run.returncode, stdout=run.stdout, stderr=run.stderr)
        cases.append(record)
        assert run.returncode == expected, record
        if args==['fail']:
            assert 'call=1 op=129 base=87654321' in run.stdout, record
            assert 'call=2 op=129 base=12345678' in run.stdout, record
            assert 'ERROR_guest_resumed' not in run.stdout, record
        else:
            assert run.stdout.count('preservation passed') in (2,4), record
            assert 'AVX_cases=' in run.stdout, record
            if args==['profile']: assert 'profile_on_preservation' in run.stdout, record
    (out / 'receipt.json').write_text(json.dumps(dict(cases=cases, actual_FS_changed=False,
        native_execution_tested=False), indent=2) + '\n')
    print(json.dumps(dict(PE_ABI_fixture_passed=True, failure_recovery_verified=True,
        actual_FS_changed=False, native_execution_tested=False)))
