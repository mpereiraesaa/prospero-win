#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Compile the patch's actual PE capability helper against the real sink."""
from pathlib import Path
import os,shlex,subprocess,tempfile

ROOT=Path(__file__).resolve().parents[1]
patch=(ROOT/'wine/patches/0472-xinput-ps5-device-capabilities.patch').read_text()
section=patch.split('+++ b/dlls/xinput1_3/main.c\n',1)[1].split('diff --git ',1)[0]
added='\n'.join(line[1:] for line in section.splitlines() if line.startswith('+'))
start=added.index('static void title_device_caps(')
helper=added[start:added.index('\n}',start)+2]
harness=r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "wine/ps5/pw_wine_sink.h"
typedef uint32_t DWORD;
typedef struct { uint16_t buttons; uint8_t lt,rt; int16_t lx,ly,rx,ry; } Gamepad;
typedef struct { uint16_t left,right; } Vibration;
typedef struct { uint8_t Type,SubType; uint16_t Flags; Gamepad Gamepad; Vibration Vibration; } Caps;
typedef struct { Caps Capabilities; uint16_t VendorId,ProductId,VersionNumber,Unknown; uint32_t Unknown2; } XINPUT_CAPABILITIES_EX;
struct xinput_caps_params { uint32_t index; PwWinePadCaps caps; };
_Static_assert(sizeof(XINPUT_CAPABILITIES_EX)==32,"PE capability ABI");
_Static_assert(sizeof(struct xinput_caps_params)==16,"pointer-free Unix ABI");
#define XINPUT_CAPS_FFB_SUPPORTED 1
enum { unix_pad_caps=3 };
static int title_pad,old_runtime;
static int __wine_unix_call(int handle,int call,void *args)
{
    struct xinput_caps_params *p=args;
    (void)handle;assert(call==unix_pad_caps);
    return old_runtime || !pw_wine_pad_caps_at(p->index,&p->caps);
}
'''+helper+r'''
int main(void)
{
    PwWinePad pad={0}; PwWinePadCaps guitar={7,0x1430,0x4748,0x3122,0};
    XINPUT_CAPABILITIES_EX caps={0};
    pw_wine_set_pad_at(0,&pad);
    assert(!pw_wine_set_pad_device_at(1,&pad,&guitar));
    memset(&caps,0xff,sizeof(caps));title_device_caps(1,&caps);
    assert(caps.Capabilities.SubType==7 && caps.VendorId==0x1430 && caps.ProductId==0x4748 && caps.VersionNumber==0x3122);
    assert(!caps.Capabilities.Flags && !caps.Capabilities.Vibration.left && !caps.Capabilities.Vibration.right);
    title_device_caps(0,&caps);
    assert(caps.Capabilities.SubType==1 && caps.VendorId==0x045e && caps.ProductId==0x028e && caps.Capabilities.Flags==1);
    old_runtime=1;caps.Capabilities.SubType=1;caps.VendorId=0x045e;
    title_device_caps(1,&caps);assert(caps.Capabilities.SubType==1 && caps.VendorId==0x045e);
    return 0;
}
'''
with tempfile.TemporaryDirectory() as tmp:
    source=Path(tmp)/'caps.c';binary=Path(tmp)/'caps'
    source.write_text(harness)
    subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=c11','-Wall','-Wextra','-Werror','-pthread','-I',str(ROOT),str(source),str(ROOT/'wine/ps5/pw_wine_sink.c'),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
print('XInput per-device PE capabilities and old-runtime fallback passed')
