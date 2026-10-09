/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_D3D9_PRIVATE_DATA_H
#define PW_D3D9_PRIVATE_DATA_H
#include <d3d9.h>
#define PW_D3D9_PRIVATE_MAX_ENTRIES 256u
#define PW_D3D9_PRIVATE_MAX_BYTES (64u * 1024u * 1024u)
struct pw_d3d9_private_entry;
/* Zero initialize. The enclosing COM method pins its object during all calls.
 * dispose is final: no concurrent method may outlive the containing object. */
struct pw_d3d9_private_data {
 SRWLOCK lock;
 struct pw_d3d9_private_entry *entries;
 unsigned count,closed;
};
HRESULT pw_d3d9_private_set(struct pw_d3d9_private_data *,REFGUID,const void *,DWORD,DWORD);
HRESULT pw_d3d9_private_get(struct pw_d3d9_private_data *,REFGUID,void *,DWORD *);
HRESULT pw_d3d9_private_free(struct pw_d3d9_private_data *,REFGUID);
void pw_d3d9_private_dispose(struct pw_d3d9_private_data *);
#endif
