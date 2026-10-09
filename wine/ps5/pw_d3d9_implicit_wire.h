/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_D3D9_IMPLICIT_WIRE_H
#define PW_D3D9_IMPLICIT_WIRE_H
#include <stddef.h>
#include <stdint.h>
#include "pw_d3d9_objects.h"
#define PW_D3D9_IMPLICIT_MAX 16u
#define PW_D3D9_IMPLICIT_REQUEST_BYTES (16u + 8u * PW_D3D9_IMPLICIT_MAX)
#define PW_D3D9_IMPLICIT_REPLY_BYTES (24u + 8u * PW_D3D9_IMPLICIT_MAX)
enum pw_d3d9_implicit_operation {
    PW_D3D9_IMPLICIT_LIST = 1, PW_D3D9_IMPLICIT_PREPARE,
    PW_D3D9_IMPLICIT_FINISH, PW_D3D9_IMPLICIT_DRAIN
};
enum pw_d3d9_implicit_disposition {
    PW_D3D9_IMPLICIT_NONE = 0, PW_D3D9_IMPLICIT_RESTORED,
    PW_D3D9_IMPLICIT_RETIRED
};
/* IDs are session registry identities, never native pointers. PREPARE lists
 * only existing frontend shells with zero public references. */
struct pw_d3d9_implicit_request {
    uint32_t operation, count;
    struct pw_d3d9_object_ref objects[PW_D3D9_IMPLICIT_MAX];
};
struct pw_d3d9_implicit_reply {
    uint32_t operation, hresult, disposition, count;
    struct pw_d3d9_object_ref objects[PW_D3D9_IMPLICIT_MAX];
};
/* Fixed length, version 1, reserved/unused words zero. Decode is atomic. */
int pw_d3d9_implicit_request_encode(void *, size_t, const struct pw_d3d9_implicit_request *);
int pw_d3d9_implicit_request_decode(struct pw_d3d9_implicit_request *, const void *, size_t);
int pw_d3d9_implicit_reply_encode(void *, size_t, const struct pw_d3d9_implicit_request *, const struct pw_d3d9_implicit_reply *);
int pw_d3d9_implicit_reply_decode(struct pw_d3d9_implicit_reply *, const struct pw_d3d9_implicit_request *, const void *, size_t);
#endif
