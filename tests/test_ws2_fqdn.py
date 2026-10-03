#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Execute old/new patch functions; optionally include pinned Wine's actual APIs."""
from pathlib import Path
import os
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PATCH = ROOT / "wine/patches/0760-ws2-32-read-fqdn-once.patch"


def function(source, name):
    match = re.search(r"(?:static char \*|BOOL WINAPI )" + name + r"\([^;]*?\)\s*\{", source)
    assert match, name
    start = source.index("{", match.start())
    end, depth = start + 1, 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


hunks = PATCH.read_text().split("\n@@", 1)[1].splitlines()[1:]
old = "\n".join(x[1:] for x in hunks if x.startswith((" ", "-")))
new = "\n".join(x[1:] for x in hunks if x.startswith((" ", "+")))
old = function(old, "get_fqdn").replace("get_fqdn(void)", "legacy_fqdn(void)")
new = function(new, "get_fqdn")

HARNESS = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
typedef wchar_t WCHAR;
typedef uint32_t DWORD;
typedef size_t SIZE_T;
typedef int BOOL, LRESULT;
typedef void *HKEY;
typedef unsigned char BYTE;
typedef enum { ComputerNameNetBIOS, ComputerNameDnsHostname, ComputerNameDnsDomain,
    ComputerNameDnsFullyQualified, ComputerNamePhysicalNetBIOS, ComputerNamePhysicalDnsHostname,
    ComputerNamePhysicalDnsDomain, ComputerNamePhysicalDnsFullyQualified } COMPUTER_NAME_FORMAT;
#define TRUE 1
#define FALSE 0
#define WINAPI
#define ERROR_MORE_DATA 234
#define ERROR_NOT_ENOUGH_MEMORY 8
#define ERROR_INVALID_PARAMETER 87
#define CP_ACP 0
#define HKEY_LOCAL_MACHINE ((void *)1)
#define KEY_READ 0
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define TRACE(...) ((void)0)
static WCHAR hostname[400], domain[100];
static DWORD last_error;
static unsigned wide_calls, registry_calls, fail_lookup, fail_lookup_on, bad_size;
static unsigned conversions, fail_conversion, allocations, fail_alloc, outstanding;
static DWORD GetLastError(void) { return last_error; }
static void SetLastError(DWORD error) { last_error=error; }
static size_t lstrlenW(const WCHAR *s) { size_t n=0; while(s[n])n++; return n; }
static WCHAR *lstrcpyW(WCHAR *d,const WCHAR *s) { WCHAR *r=d; while((*d++=*s++)) {} return r; }
static BOOL sameW(const WCHAR *a,const WCHAR *b) { while(*a && *a==*b){a++;b++;}return *a==*b; }
static int RegOpenKeyExW(HKEY parent,const WCHAR *key,DWORD reserved,DWORD access,HKEY *out)
{ assert(parent==HKEY_LOCAL_MACHINE);registry_calls++;*out=(void *)2;return 0; }
static int RegQueryValueExW(HKEY key,const WCHAR *value,DWORD *reserved,DWORD *type,BYTE *data,DWORD *size)
{
    const WCHAR *s=sameW(value,L"Hostname")?hostname:domain;
    DWORD needed=(DWORD)((lstrlenW(s)+1)*sizeof(WCHAR));
    if(!data || *size<needed){*size=needed;return ERROR_MORE_DATA;}
    memcpy(data,s,needed);*size=needed;return 0;
}
static int RegCloseKey(HKEY key) { return 0; }
static void *GetProcessHeap(void) { return NULL; }
static void *HeapAlloc(void *heap,DWORD flags,size_t size) { return malloc(size?size:1); }
static BOOL HeapFree(void *heap,DWORD flags,void *ptr) { free(ptr);return TRUE; }
static int WideCharToMultiByte(unsigned cp,DWORD flags,const WCHAR *s,int len,char *out,
                               int capacity,const char *def,BOOL *used)
{
    char bytes[2000];size_t n=0;
    assert(cp==CP_ACP && !flags && len==-1 && !def && !used);
    conversions++;if(conversions==fail_conversion)return 0;
    while(*s){unsigned c=*s++;
        if(c>=0xd800 && c<=0xdbff){assert(*s>=0xdc00 && *s<=0xdfff);c=0x10000+((c-0xd800)<<10)+(*s++-0xdc00);}
        if(c<0x80)bytes[n++]=(char)c;
        else if(c<0x800){bytes[n++]=(char)(0xc0|(c>>6));bytes[n++]=(char)(0x80|(c&63));}
        else if(c<0x10000){bytes[n++]=(char)(0xe0|(c>>12));bytes[n++]=(char)(0x80|((c>>6)&63));bytes[n++]=(char)(0x80|(c&63));}
        else{bytes[n++]=(char)(0xf0|(c>>18));bytes[n++]=(char)(0x80|((c>>12)&63));bytes[n++]=(char)(0x80|((c>>6)&63));bytes[n++]=(char)(0x80|(c&63));}
    }
    bytes[n++]=0;if(out){if(capacity<(int)n)return 0;memcpy(out,bytes,n);}return (int)n;
}
static BOOL GetComputerNameExW(COMPUTER_NAME_FORMAT type,WCHAR *name,DWORD *len);
/*API_FUNCTIONS*/
static BOOL GetComputerNameExW(COMPUTER_NAME_FORMAT type,WCHAR *name,DWORD *len)
{
    assert(type==ComputerNamePhysicalDnsFullyQualified);wide_calls++;
    if(fail_lookup || wide_calls==fail_lookup_on){SetLastError(5);return FALSE;}
    if(bad_size){*len=0;SetLastError(ERROR_MORE_DATA);return FALSE;}
#ifdef REAL_API
    return wine_GetComputerNameExW(type,name,len);
#else
    size_t h=lstrlenW(hostname),d=lstrlenW(domain),n=h+(d?d+1:0);
    registry_calls+=2;
    if(!name || *len<=n){*len=(DWORD)(n+1);SetLastError(ERROR_MORE_DATA);return FALSE;}
    memcpy(name,hostname,h*sizeof(WCHAR));if(d){name[h]='.';memcpy(name+h+1,domain,d*sizeof(WCHAR));}
    name[n]=0;*len=(DWORD)n;return TRUE;
#endif
}
#ifndef REAL_API
/* Wine's ANSI adapter also fetches the wide name twice, then converts CP_ACP. */
static BOOL GetComputerNameExA(COMPUTER_NAME_FORMAT type,char *name,DWORD *len)
{
    DWORD n=0;WCHAR *wide;int count;BOOL ok=FALSE;
    GetComputerNameExW(type,NULL,&n);if(GetLastError()!=ERROR_MORE_DATA)return FALSE;
    wide=malloc(n*sizeof(*wide));if(!wide)return FALSE;
    if(GetComputerNameExW(type,wide,&n)){
        count=WideCharToMultiByte(CP_ACP,0,wide,-1,NULL,0,NULL,NULL);
        if(count>(int)*len){*len=(DWORD)count;SetLastError(ERROR_MORE_DATA);}
        else{WideCharToMultiByte(CP_ACP,0,wide,-1,name,(int)*len,NULL,NULL);*len=(DWORD)(count-1);ok=TRUE;}
    }
    free(wide);return ok;
}
#endif
static void *tracked_malloc(size_t bytes)
{ void *p;allocations++;if(allocations==fail_alloc || bytes>2000)return NULL;p=malloc(bytes);if(p)outstanding++;return p; }
static void tracked_free(void *p) { if(p){assert(outstanding);outstanding--;free(p);} }
#define malloc tracked_malloc
#define free tracked_free
/*PATCH_FUNCTIONS*/
#undef malloc
#undef free
static void reset(void)
{
    assert(!outstanding);wide_calls=registry_calls=conversions=allocations=0;
    fail_lookup=fail_lookup_on=bad_size=fail_conversion=fail_alloc=0;last_error=0;
    lstrcpyW(hostname,L"alpha");lstrcpyW(domain,L"example");
}
static void expect(const char *text,unsigned calls)
{ char *s=get_fqdn();assert(s && !strcmp(s,text));assert(wide_calls==calls);tracked_free(s);assert(!outstanding); }
static void compare(const char *text)
{
    char *old_name,*new_name;reset();old_name=legacy_fqdn();assert(old_name && !strcmp(old_name,text));
    assert(wide_calls==4 && registry_calls==8);tracked_free(old_name);
    wide_calls=registry_calls=conversions=allocations=0;new_name=get_fqdn();assert(new_name && !strcmp(new_name,text));
    assert(wide_calls==1 && registry_calls==2);tracked_free(new_name);
}
int main(void)
{
    _Static_assert(sizeof(WCHAR)==2,"Wine uses UTF-16");
    compare("alpha.example");
    reset();expect("alpha.example",1);wide_calls=0;lstrcpyW(hostname,L"beta");expect("beta.example",1);
    reset();hostname[0]=0x00f1;hostname[1]=0x548c;hostname[2]=0xd83d;hostname[3]=0xde42;hostname[4]=0;domain[0]=0;
    {char *old_name=legacy_fqdn(),*new_name=get_fqdn();assert(old_name&&new_name&&!strcmp(old_name,new_name));assert(!strcmp(new_name,"\xc3\xb1\xe5\x92\x8c\xf0\x9f\x99\x82"));tracked_free(old_name);tracked_free(new_name);}
    reset();hostname[0]=domain[0]=0;expect("",1);
    reset();for(unsigned i=0;i<200;i++)hostname[i]=0x548c;hostname[200]=domain[0]=0;
    {char *s=get_fqdn();assert(s&&strlen(s)==600&&wide_calls==1);tracked_free(s);}
    reset();fail_lookup=1;assert(!get_fqdn()&&!allocations);
    reset();bad_size=1;assert(!get_fqdn()&&!allocations);
    reset();fail_alloc=1;assert(!get_fqdn()&&!outstanding);
    reset();fail_conversion=1;assert(!get_fqdn()&&!allocations);
    reset();fail_conversion=2;assert(!get_fqdn()&&!outstanding);
#ifndef REAL_API
    reset();for(unsigned i=0;i<300;i++)hostname[i]='x';hostname[300]=domain[0]=0;
    {char *s=get_fqdn();assert(s&&strlen(s)==300&&wide_calls==2);tracked_free(s);}
    reset();for(unsigned i=0;i<300;i++)hostname[i]='x';hostname[300]=domain[0]=0;fail_lookup_on=2;assert(!get_fqdn()&&!outstanding);
    reset();for(unsigned i=0;i<300;i++)hostname[i]='x';hostname[300]=domain[0]=0;fail_alloc=1;assert(!get_fqdn()&&!outstanding);
    reset();for(unsigned i=0;i<300;i++)hostname[i]='x';hostname[300]=domain[0]=0;fail_alloc=2;assert(!get_fqdn()&&!outstanding);
#endif
    puts("fqdn passed: fresh lookup, 8->2 registry opens, encoding, growth and error cleanup");return 0;
}
'''


def run(api="", real=False):
    code = HARNESS.replace("/*API_FUNCTIONS*/", api).replace("/*PATCH_FUNCTIONS*/", old + "\n" + new)
    with tempfile.TemporaryDirectory() as directory:
        source, exe = Path(directory) / "fqdn.c", Path(directory) / "fqdn"
        source.write_text(code)
        command = shlex.split(os.environ.get("CC", "cc")) + shlex.split(os.environ.get("CFLAGS", "-O2"))
        command += ["-std=gnu11", "-fshort-wchar", "-Wno-unused-function", "-Wno-unused-parameter"]
        if real:
            command += ["-DREAL_API"]
        subprocess.run(command + [str(source), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


run()
if path := os.environ.get("PROSPERO_WINE_SOURCE"):
    pinned = Path(path)
    protocol = (pinned / "dlls/ws2_32/protocol.c").read_text()
    current = function(protocol, "get_fqdn")
    assert current == new or current.replace("get_fqdn(void)", "legacy_fqdn(void)") == old
    registry = (pinned / "dlls/kernelbase/registry.c").read_text()
    wide = function(registry, "GetComputerNameExW").replace("GetComputerNameExW", "wine_GetComputerNameExW")
    ansi = function(registry, "GetComputerNameExA")
    run(wide + "\n" + ansi, real=True)
    print("fqdn actual pinned Wine ANSI/wide registry functions passed")
