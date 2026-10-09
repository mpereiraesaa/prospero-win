#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Compile exact Wine ancestor/rectangle functions with controlled desktop state."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--wine-source', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
p.add_argument("--wine-build", type=Path)
p.add_argument("--prefix", type=Path)
p.add_argument("--expect-worker-failure", action="store_true")
a = p.parse_args()
out = a.output.resolve()
out.mkdir(parents=True, exist_ok=False)
repo = Path(__file__).resolve().parents[2]
patch = repo / 'wine/patches/0910-win32u-ps5-worker-desktop-cache.patch'
receipt = {'status': 'running', 'commands': [], 'sources': {str(Path(__file__).resolve()): hashlib.sha256(Path(__file__).read_bytes()).hexdigest()}}

def run(cmd, name):
    r = subprocess.run(list(map(str, cmd)), capture_output=True, text=True, timeout=60)
    (out / (name + '.log')).write_text(r.stdout + r.stderr)
    receipt['commands'].append({'command': list(map(str, cmd)), 'exit': r.returncode})
    if r.returncode:
        raise RuntimeError(name)

def function(s, signature):
    start = s.index(signature)
    begin = s.index('{', start)
    depth = 1
    end = begin + 1
    while depth:
        depth += (s[end] == '{') - (s[end] == '}')
        end += 1
    return s[start:end] + '\n'

try:
    for f in ['window.c', 'winstation.c', 'win32u_private.h', 'ntuser_private.h']:
        src = a.wine_source.resolve() / 'dlls/win32u' / f
        receipt['sources'][str(src)] = hashlib.sha256(src.read_bytes()).hexdigest()
        target = out / 'source/dlls/win32u' / f
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(src.read_bytes())
    receipt['sources'][str(patch)] = hashlib.sha256(patch.read_bytes()).hexdigest()
    baseline = (out / 'source/dlls/win32u/window.c').read_text()
    run(['patch', '-d', out / 'source', '-p1', '--input', patch], 'apply')
    window = (out / 'source/dlls/win32u/window.c').read_text()
    station = (out / 'source/dlls/win32u/winstation.c').read_text()
    prelude = r'''
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
typedef uintptr_t HWND;
typedef int BOOL;
typedef uint32_t user_handle_t;
typedef struct { int left,top,right,bottom; } RECT;
typedef struct { int x,y; } POINT;
typedef struct { HWND parent; } WND;
struct ratio { int num,den; };
struct window_rects { RECT window,client,visible; };
struct user_thread_info { HWND top_window,msg_window; BOOL desktop_cache_only; };
static struct user_thread_info info;
static WND service={0x10024};
static int server_calls, server_fail, held, rect_fail, map_calls;
static int desktop_callbacks, builtin_callbacks;
static HWND get_desktop_window(void);
static void set_desktop(HWND hwnd){desktop_callbacks++;assert(hwnd==0x10024);assert(get_desktop_window()==hwnd);}
static void register_builtin_classes(void){builtin_callbacks++;assert(get_desktop_window()==0x10024);}
static struct { void (*pSetDesktopWindow)(HWND); } driver={set_desktop},*user_driver=&driver;
#define ERR_(channel) printf
#define TRUE 1
#define FALSE 0
#define GA_ROOT 2
#define COORDS_PARENT 0
#define WINE_PS5_USER_DRIVER 1
#define WND_OTHER_PROCESS ((WND *)(uintptr_t)-1)
#define WND_DESKTOP ((WND *)(uintptr_t)-2)
#define NTUSER_OBJ_WINDOW 1
#define LOWORD(x) ((unsigned short)(x))
#define HIWORD(x) ((unsigned short)((x)>>16))
struct req_type { int force; HWND handle; };
struct reply_type { HWND top_window,msg_window; int count; };
#define SERVER_START_REQ(x) do { struct req_type req_storage={0},*req=&req_storage; struct reply_type reply_storage={0x10024,0x10026,0},*reply=&reply_storage;
#define SERVER_END_REQ } while(0)
static int wine_server_call(struct req_type *r){ assert(!held); assert(!r->force); server_calls++; return server_fail; }
#define wine_server_ptr_handle(x) ((HWND)(x))
#define wine_server_user_handle(x) (x)
#define wine_server_set_reply(a,b,c) ((void)(a),(void)(b),(void)(c))
static struct user_thread_info *get_user_thread_info(void){return &info;}
static WND *get_user_handle_ptr(HWND hwnd,int type){(void)type;if(hwnd==0x1003e){held++;return &service;}return NULL;}
static void release_win_ptr(WND *w){assert(w==&service&&held==1);held--;}
static HWND get_full_window_handle(HWND h){return h;}
'''
    common = ''.join(function(window, signature) for signature in [
        'BOOL is_desktop_window(', 'WND *get_win_ptr(', 'static HWND *list_window_parents('])
    root_case = window.split('    case GA_ROOT:\n', 1)[1].split('    case GA_ROOTOWNER:', 1)[0]
    root_case = root_case.rsplit('        break;', 1)[0]
    ancestor = 'static HWND NtUserGetAncestor(HWND hwnd,int kind){HWND *list,ret=0;(void)kind;\n' + root_case + '\nreturn ret;}\n'
    mocks = r'''
static struct ratio get_dpi_for_window(HWND h){(void)h;return (struct ratio){1,1};}
static BOOL get_window_rects(HWND h,int rel,struct window_rects *r,struct ratio dpi){(void)rel;(void)dpi;if(!h||rect_fail)return 0;r->client=r->visible=r->window=(RECT){0,0,1920,1080};return 1;}
static struct window_rects map_window_rects_virt_to_raw(struct window_rects r,struct ratio d){(void)d;map_calls++;return r;}
static BOOL get_present_rect(HWND h,RECT *r,struct ratio d){(void)h;(void)d;*r=(RECT){0,0,1920,1080};return 1;}
static BOOL get_client_rect(HWND h,RECT *r,struct ratio d){return get_present_rect(h,r,d);}
static void map_window_points(HWND a,HWND b,POINT *p,int n,struct ratio d){(void)a;(void)b;(void)p;(void)n;(void)d;}
static void get_win_monitor_dpi(HWND h,struct ratio *d){(void)h;*d=(struct ratio){1,1};}
static RECT map_dpi_rect(RECT r,struct ratio a,struct ratio b){(void)a;(void)b;return r;}
static void OffsetRect(RECT *r,int x,int y){r->left+=x;r->right+=x;r->top+=y;r->bottom+=y;}
static void SetRectEmpty(RECT *r){memset(r,0,sizeof(*r));}
'''
    reset_start = station.index('        thread_info->top_window = 0;')
    reset_end = station.index('        if (was_virtual_desktop', reset_start)
    reset = 'static void reset_cache(void){struct user_thread_info *thread_info=&info;\n' + station[reset_start:reset_end] + '}\n'
    tests = r'''
int main(void){
 HWND *parents; RECT r,m;
 /* Exact old ancestor walk cannot recognize a server-only desktop. */
 parents=list_window_parents(0x1003e);assert(!parents&&!held&&!server_calls);
 assert(NtUserGetAncestor(0x1003e,GA_ROOT)==0x1003e);assert(server_calls==1&&!held);
 assert(NtUserGetAncestor(0x1003e,GA_ROOT)==0x1003e&&server_calls==1);
 assert(info.desktop_cache_only&&!desktop_callbacks&&!builtin_callbacks);
 assert(get_desktop_window()==0x10024&&!info.desktop_cache_only);
 assert(desktop_callbacks==1&&builtin_callbacks==1);
 assert(get_desktop_window()==0x10024&&desktop_callbacks==1&&builtin_callbacks==1);
 r=get_client_surface_rects(0,0x1003e,&m);assert(r.right==1920&&r.bottom==1080&&!r.left&&!r.top);
 rect_fail=1;map_calls=0;memset(&m,0xa5,sizeof(m));r=get_client_surface_rects(0x1003e,0x1003e,&m);
 assert(!memcmp(&r,&(RECT){0},sizeof(r))&&!memcmp(&m,&(RECT){0},sizeof(m))&&!map_calls);
 rect_fail=0;reset_cache();assert(!info.top_window&&!info.msg_window&&!info.desktop_cache_only);server_fail=1;map_calls=0;memset(&m,0xa5,sizeof(m));
 r=get_client_surface_rects(0,0x1003e,&m);assert(!memcmp(&r,&(RECT){0},sizeof(r))&&!memcmp(&m,&(RECT){0},sizeof(m))&&!map_calls);
 server_fail=0;assert(NtUserGetAncestor(0x1003e,GA_ROOT)==0x1003e);assert(!held);
 assert(info.desktop_cache_only);assert(get_desktop_window()==0x10024);
 assert(desktop_callbacks==2&&builtin_callbacks==2);
 puts("worker desktop: negative walk, cache/retry, valid extent, zero failure outputs PASS");return 0;
}
'''
    full = function(station, 'HWND get_desktop_window(void)')
    prefix = full.split('    /* don\'t create', 1)[0].replace('HWND get_desktop_window', 'static HWND get_desktop_window').replace('    BOOL is_service;\n', '')
    tail = full[full.index('initialize:'):]
    lazy = prefix + 'assert(!"unexpected uncached full-init path"); return 0;\n' + tail
    code = prelude + function(station, 'void cache_thread_desktop_windows(') + lazy + common + ancestor + mocks + function(window, 'static RECT get_client_surface_rects(') + reset + tests
    source = out / 'contract.c'
    source.write_text(code)
    for name, flags in [('plain', []), ('sanitize', ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'])]:
        run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', *flags, source, '-o', out / name], 'compile-' + name)
        run([out / name], name)
    receipt['generated_source_sha256'] = hashlib.sha256(source.read_bytes()).hexdigest()
    receipt['scope'] = 'Exact extracted Wine functions with controlled server-only desktop/failed rectangle state; not full Wine runtime execution.'
    if a.wine_build:
        if not a.prefix:
            raise RuntimeError('--prefix required with --wine-build')
        receipt['runtime'] = {str(a.wine_build.resolve() / name): hashlib.sha256((a.wine_build / name).read_bytes()).hexdigest() for name in ['dlls/win32u/win32u.so', 'dlls/ntdll/ntdll.so']}
        native_source = Path(__file__).with_suffix('.c')
        receipt['sources'][str(native_source)] = hashlib.sha256(native_source.read_bytes()).hexdigest()
        for arch, target, extra in [('i686', 'client.exe', []), ('x86_64', 'service.dll', ['-shared'])]:
            run([arch + '-w64-mingw32-gcc', '-O2', '-Wall', '-Wextra', '-Werror', *extra, native_source, '-luser32', '-o', out / target], 'build-' + arch)
        env = os.environ.copy()
        env.update(WINEPREFIX=str(a.prefix.resolve()), WINEDEBUG='-all', WINE_PS5_DESKTOP='1920x1080', WINEDLLOVERRIDES='explorer.exe=d;winemenubuilder.exe=d;mscoree,mshtml=')
        service_path = 'Z:' + str(out / 'service.dll').replace('/', chr(92))
        for i in range(3):
            command = [str(a.wine_build.resolve() / 'loader/wine'), str(out / 'client.exe'), service_path]
            with (out / ('native-' + str(i) + '.log')).open('w') as log:
                result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, env=env, timeout=60)
            receipt['commands'].append({'command': command, 'exit': result.returncode})
            expected = 1 if a.expect_worker_failure else 0
            if result.returncode != expected:
                raise RuntimeError('native process exit ' + str(result.returncode))
            text = (out / ('native-' + str(i) + '.log')).read_text()
            if a.expect_worker_failure:
                assert 'bits=64 root=0000000000000000' in text, text
            else:
                assert 'PW_WORKER_NATIVE status=00000000 result=0 size=592' in text, text
        receipt['native_scope'] = 'Three PE32 guest and native PE64 fresh-worker ancestor/client-extent runs; PS5 driver overlay identity must be recorded separately.'
        receipt['native_expected_failure'] = a.expect_worker_failure
    receipt['status'] = 'pass'
except BaseException as error:
    receipt['status'] = 'timeout' if isinstance(error, subprocess.TimeoutExpired) else 'failed'
    receipt['error'] = str(error)
    raise
finally:
    (out / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
