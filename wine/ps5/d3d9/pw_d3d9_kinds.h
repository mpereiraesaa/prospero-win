/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_D3D9_KINDS_H
#define PW_D3D9_KINDS_H
/* Service-local registry tags. Object IDs are always accompanied by generation;
 * these tags validate the interface before any native COM cast. */
enum pw_d3d9_object_kind {
 PW_D3D9_KIND_FACTORY=1, PW_D3D9_KIND_DEVICE=2,
 PW_D3D9_KIND_VERTEX_BUFFER=3, PW_D3D9_KIND_INDEX_BUFFER=4,
 PW_D3D9_KIND_TEXTURE_2D=5, PW_D3D9_KIND_SURFACE=6,
 PW_D3D9_KIND_VERTEX_DECLARATION=7, PW_D3D9_KIND_VERTEX_SHADER=8,
 PW_D3D9_KIND_PIXEL_SHADER=9, PW_D3D9_KIND_STATE_BLOCK=10,
 PW_D3D9_KIND_QUERY=11
};
#endif
