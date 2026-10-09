/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Compile and execute independently as PE32 and PE64 against native headers. */
#define CINTERFACE
#include <stddef.h>
#include <stdio.h>
#include <windows.h>
#include <d3d9.h>
#include "../../wine/ps5/d3d9/pw_d3d9_inventory.h"
#define CHECK_METHOD(iface, slot, name, returned, args, policy) \
    typedef returned (STDMETHODCALLTYPE *signature_##iface##_##name) args; \
    _Static_assert(offsetof(iface##Vtbl, name) == (slot) * sizeof(void *), "vtable slot"); \
    _Static_assert(__builtin_types_compatible_p(__typeof__(((iface##Vtbl *)0)->name), \
        signature_##iface##_##name), "method signature");
PW_D3D9_ALL_METHODS(CHECK_METHOD)
#undef CHECK_METHOD
#define CHECK_INTERFACE(iface, count) \
    _Static_assert(sizeof(iface##Vtbl) == (count) * sizeof(void *), "vtable size");
PW_D3D9_INTERFACES(CHECK_INTERFACE)
#undef CHECK_INTERFACE
#define COUNT_METHOD(iface, slot, name, returned, args, policy) +1
enum { method_count = 0 PW_D3D9_ALL_METHODS(COUNT_METHOD) };
#undef COUNT_METHOD
int main(void)
{
    printf("D3D9 inventory: PASS interfaces=17 methods=%u pointer_bytes=%u\n",
           method_count, (unsigned)sizeof(void *));
    return 0;
}
