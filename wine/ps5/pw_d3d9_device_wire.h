/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_D3D9_DEVICE_WIRE_H
#define PW_D3D9_DEVICE_WIRE_H
#include <stddef.h>
#include <stdint.h>
#include "pw_d3d9_window.h"
#include "pw_d3d9_objects.h"
#define PW_D3D9_DEVICE_VERSION 1u
#define PW_D3D9_DEVICE_MAX_DIRTY_RECTS 256u
#define PW_D3D9_DEVICE_MAX_REQUEST (88u + 16u * PW_D3D9_DEVICE_MAX_DIRTY_RECTS)
#define PW_D3D9_DEVICE_MAX_REPLY 88u
/* Explicit operation namespace: factory CreateDevice and device Reset both
 * occupy COM slot16. The adapter validates the target interface separately. */
enum pw_d3d9_device_operation {
    PW_D3D9_DEVICE_CREATE = 1, PW_D3D9_DEVICE_RESET, PW_D3D9_DEVICE_PRESENT
};
enum pw_d3d9_device_codec_result {
    PW_D3D9_DEVICE_OK, PW_D3D9_DEVICE_INVALID, PW_D3D9_DEVICE_SMALL,
    PW_D3D9_DEVICE_UNSUPPORTED
};
#define PW_D3D9_PRESENT_FIELDS(X) \
    X(width, BackBufferWidth) \
    X(height, BackBufferHeight) \
    X(format, BackBufferFormat) \
    X(count, BackBufferCount) \
    X(multisample_type, MultiSampleType) \
    X(multisample_quality, MultiSampleQuality) \
    X(swap_effect, SwapEffect) \
    X(windowed, Windowed) \
    X(auto_depth_stencil, EnableAutoDepthStencil) \
    X(depth_stencil_format, AutoDepthStencilFormat) \
    X(flags, Flags) \
    X(refresh_rate, FullScreen_RefreshRateInHz) \
    X(interval, PresentationInterval)
struct pw_d3d9_present_parameters {
#define PW_D3D9_PP_FIELD(name, native) uint32_t name;
    PW_D3D9_PRESENT_FIELDS(PW_D3D9_PP_FIELD)
#undef PW_D3D9_PP_FIELD
    struct pw_d3d9_window_id window;
};
struct pw_d3d9_rect { int32_t left, top, right, bottom; };
#define PW_D3D9_PRESENT_SOURCE 1u
#define PW_D3D9_PRESENT_DESTINATION 2u
#define PW_D3D9_PRESENT_DIRTY 4u
struct pw_d3d9_device_request {
    uint32_t operation, adapter, device_type, behavior_flags;
    struct pw_d3d9_window_id focus_window;
    struct pw_d3d9_present_parameters parameters;
    uint32_t present_fields, dirty_count;
    struct pw_d3d9_window_id override_window;
    struct pw_d3d9_rect source, destination, dirty_bounds;
    struct pw_d3d9_rect dirty[PW_D3D9_DEVICE_MAX_DIRTY_RECTS];
};
struct pw_d3d9_device_reply {
    uint32_t operation, hresult;
    struct pw_d3d9_object_ref object;
    struct pw_d3d9_present_parameters parameters;
};
/* DTOs are local storage, never native layout/wire memcpy. All window IDs are
 * driver-issued epoch/id/generation triples, or all zero for a null HWND.
 * CREATE/RESET always return the backend's in/out parameters, even on failure.
 * The adapter translates local windows and must never serialize an HWND.
 * Calls beyond the bounded dirty rectangle limit are explicitly unsupported.
 * Encode/decode publish only on success; inputs and outputs must not overlap. */
int pw_d3d9_device_request_encode(void *, size_t, size_t *, const struct pw_d3d9_device_request *);
int pw_d3d9_device_request_decode(struct pw_d3d9_device_request *, const void *, size_t);
int pw_d3d9_device_reply_encode(void *, size_t, size_t *, const struct pw_d3d9_device_reply *);
int pw_d3d9_device_reply_decode(struct pw_d3d9_device_reply *, const void *, size_t);
#endif
