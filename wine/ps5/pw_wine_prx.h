/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_WINE_PRX_H
#define PW_WINE_PRX_H
#include <stddef.h>
#include <stdint.h>

/* PRX export descriptors for Wine's Unix modules on PS5. On firmware 12.02
 * sceKernelDlsym does not resolve an application PRX's exports and
 * module_start is not run, so each module carries a static, relocated table
 * of {name, address} pairs that the loader finds by scanning the module's
 * segments. The layout is the PRXDESC1 format the console already accepts
 * for application modules: "PRXDESC1", version 1, a count, then the pairs,
 * 16-byte aligned. Everything here is pure and host-testable. */
#define PW_PRX_MAGIC UINT64_C(0x3143534544585250) /* "PRXDESC1" little-endian */
enum {
    PW_PRX_VERSION=1,PW_PRX_ALIGN=16,PW_PRX_MAX_EXPORTS=4096,PW_PRX_MAX_SEGMENTS=4,
    PW_PRX_MAX_NAME=256,PW_PRX_MODULE_INFO_BYTES=0x160,
    PW_PRX_OK=0,PW_PRX_ERR_ARGUMENT=-1,PW_PRX_ERR_MODULE_INFO=-2,PW_PRX_ERR_NO_DESCRIPTOR=-3
};
typedef struct PwPrxExport { const char *name; const void *address; } PwPrxExport;
typedef struct PwPrxDescriptor {
    uint64_t magic;uint32_t version,count;
    PwPrxExport exports[];
} PwPrxDescriptor;
typedef struct PwPrxSegment { const void *address;uint32_t size,protection; } PwPrxSegment;

/* Parses a SceKernelModuleInfo record: 64-bit size at 0 (0x160 as the
 * caller set it, or 0: the console clears it on success), name at 8,
 * up to four {address, 32-bit size, 32-bit protection} segments at 0x108 and
 * their count at 0x148. */
int pw_prx_parse_module_info(const void *info,char name[PW_PRX_MAX_NAME],
                             PwPrxSegment segments[PW_PRX_MAX_SEGMENTS],uint32_t *count);
/* Finds the first valid descriptor in the readable segments. A candidate is
 * valid only if the whole table, every name (terminated within 256 bytes)
 * and every address lie inside the module's own segments. */
int pw_prx_find_descriptor(const PwPrxSegment *segments,uint32_t count,
                           const PwPrxDescriptor **descriptor);
const void *pw_prx_lookup(const PwPrxDescriptor *descriptor,const char *name);

#endif
