/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include "wine/ps5/pw_d3d9_device_wire.h"
#ifdef _WIN32
#include <windows.h>
#include <d3d9.h>
#endif

static void put32(unsigned char *p, uint32_t n)
{
    unsigned i;
    for (i = 0; i < 4; ++i) p[i] = (unsigned char)(n >> (8 * i));
}
static void parameters(struct pw_d3d9_present_parameters *p)
{
    uint32_t n = 1;
#define SET_FIELD(name, native) p->name = UINT32_C(0x81230000) + n++;
    PW_D3D9_PRESENT_FIELDS(SET_FIELD)
#undef SET_FIELD
    p->windowed = 1; p->auto_depth_stencil = 0;
    p->window = (struct pw_d3d9_window_id){9, 10, 11};
#ifdef _WIN32
    {
        D3DPRESENT_PARAMETERS native = {0};
        struct pw_d3d9_present_parameters back = {0};
        /* Field mapping deliberately skips HWND; native layouts differ. */
#define TO_NATIVE(name, member) assert(sizeof(native.member) == 4); memcpy(&native.member, &p->name, 4);
        PW_D3D9_PRESENT_FIELDS(TO_NATIVE)
#undef TO_NATIVE
#define FROM_NATIVE(name, member) memcpy(&back.name, &native.member, 4); assert(back.name == p->name);
        PW_D3D9_PRESENT_FIELDS(FROM_NATIVE)
#undef FROM_NATIVE
        assert(sizeof(native.hDeviceWindow) == sizeof(void *));
    }
#endif
}
static void request_roundtrip(const struct pw_d3d9_device_request *r, size_t expected)
{
    struct pw_d3d9_device_request out, marker;
    unsigned char wire[PW_D3D9_DEVICE_MAX_REQUEST + 1], again[sizeof(wire)];
    size_t n, m, i;
    memset(wire, 0xa5, sizeof(wire));
    assert(!pw_d3d9_device_request_encode(wire, sizeof(wire), &n, r));
    assert(n == expected && wire[n] == 0xa5);
    assert(!pw_d3d9_device_request_decode(&out, wire, n));
    assert(!pw_d3d9_device_request_encode(again, sizeof(again), &m, &out));
    assert(n == m && !memcmp(wire, again, n));
    memset(&marker, 0x5a, sizeof(marker));
    for (i = 0; i < n; ++i)
    {
        out = marker;
        assert(pw_d3d9_device_request_decode(&out, wire, i));
        assert(!memcmp(&out, &marker, sizeof(out)));
    }
    assert(pw_d3d9_device_request_decode(&out, wire, n + 1));
    memset(again, 0x5a, sizeof(again));
    assert(pw_d3d9_device_request_encode(again, n - 1, &m, r) == PW_D3D9_DEVICE_SMALL && m == n);
    for (i = 0; i < sizeof(again); ++i) assert(again[i] == 0x5a);
    put32(wire + 12, 1); assert(pw_d3d9_device_request_decode(&out, wire, n));
}
static void reply_roundtrip(const struct pw_d3d9_device_reply *r, size_t expected)
{
    struct pw_d3d9_device_reply out, marker;
    unsigned char wire[89], again[89];
    size_t n, m, i;
    memset(wire, 0xa5, sizeof(wire));
    assert(!pw_d3d9_device_reply_encode(wire, sizeof(wire), &n, r));
    assert(n == expected && wire[n] == 0xa5);
    assert(!pw_d3d9_device_reply_decode(&out, wire, n));
    assert(out.hresult == r->hresult && out.object.id == r->object.id);
    if (r->operation != PW_D3D9_DEVICE_PRESENT) assert(out.parameters.width == r->parameters.width);
    assert(!pw_d3d9_device_reply_encode(again, sizeof(again), &m, &out));
    assert(n == m && !memcmp(wire, again, n));
    memset(&marker, 0x5a, sizeof(marker));
    for (i = 0; i < n; ++i)
    {
        out = marker;
        assert(pw_d3d9_device_reply_decode(&out, wire, i));
        assert(!memcmp(&out, &marker, sizeof(out)));
    }
    assert(pw_d3d9_device_reply_decode(&out, wire, n + 1));
    memset(again, 0x5a, sizeof(again));
    assert(pw_d3d9_device_reply_encode(again, n - 1, &m, r) == PW_D3D9_DEVICE_SMALL && m == n);
    for (i = 0; i < sizeof(again); ++i) assert(again[i] == 0x5a);
}
int main(void)
{
    struct pw_d3d9_device_request r = {0}, out;
    struct pw_d3d9_device_reply reply = {0}, decoded;
    unsigned char wire[PW_D3D9_DEVICE_MAX_REQUEST];
    size_t n;
    unsigned i;
    r.operation = PW_D3D9_DEVICE_CREATE; r.adapter = 17; r.device_type = 1; r.behavior_flags = 0x40;
    r.focus_window = (struct pw_d3d9_window_id){3, 4, 5}; parameters(&r.parameters);
    request_roundtrip(&r, 104);
    assert(!pw_d3d9_device_request_encode(wire, sizeof(wire), &n, &r));
    assert(wire[8] == 88 && wire[16] == 17 && wire[28] == 3 && wire[40] == 1 && wire[43] == 0x81);
    r.focus_window.id = 0; assert(pw_d3d9_device_request_encode(wire, sizeof(wire), &n, &r) == PW_D3D9_DEVICE_INVALID);
    r.focus_window = (struct pw_d3d9_window_id){0}; request_roundtrip(&r, 104);
    r.operation = PW_D3D9_DEVICE_RESET; request_roundtrip(&r, 80);
    r.parameters.windowed = 2; assert(pw_d3d9_device_request_encode(wire, sizeof(wire), &n, &r) == PW_D3D9_DEVICE_INVALID);
    r.parameters.windowed = 1; r.parameters.auto_depth_stencil = UINT32_MAX;
    assert(pw_d3d9_device_request_encode(wire, sizeof(wire), &n, &r) == PW_D3D9_DEVICE_INVALID);
    r.parameters.auto_depth_stencil = 0; r.parameters.window.epoch = 0;
    assert(pw_d3d9_device_request_encode(wire, sizeof(wire), &n, &r) == PW_D3D9_DEVICE_INVALID);
    r.parameters.window = (struct pw_d3d9_window_id){0}; request_roundtrip(&r, 80);
    assert(!pw_d3d9_device_request_encode(wire, sizeof(wire), &n, &r));
    put32(wire + 16 + 28, 3); assert(pw_d3d9_device_request_decode(&out, wire, n));

    memset(&r, 0, sizeof(r)); r.operation = PW_D3D9_DEVICE_PRESENT;
    r.source = (struct pw_d3d9_rect){-100, INT32_MIN, INT32_MAX, 200};
    r.destination = (struct pw_d3d9_rect){10, 20, 30, 40};
    r.dirty_bounds = r.source;
    for (i = 0; i < 8; ++i) { r.present_fields = i; request_roundtrip(&r, 88); }
    r.override_window = (struct pw_d3d9_window_id){7, 8, 9};
    r.present_fields = 7; r.dirty_count = PW_D3D9_DEVICE_MAX_DIRTY_RECTS;
    for (i = 0; i < r.dirty_count; ++i) r.dirty[i] = (struct pw_d3d9_rect){-(int32_t)i, 1, (int32_t)i, 100};
    request_roundtrip(&r, PW_D3D9_DEVICE_MAX_REQUEST);
    assert(!pw_d3d9_device_request_encode(wire, sizeof(wire), &n, &r));
    assert(wire[32] == 0x9c && wire[35] == 0xff && wire[39] == 0x80);
    put32(wire + 64, UINT32_MAX); assert(pw_d3d9_device_request_decode(&out, wire, n));
    ++r.dirty_count; assert(pw_d3d9_device_request_encode(wire, sizeof(wire), &n, &r) == PW_D3D9_DEVICE_UNSUPPORTED);
    r.dirty_count = 1; r.present_fields = 0;
    assert(pw_d3d9_device_request_encode(wire, sizeof(wire), &n, &r) == PW_D3D9_DEVICE_INVALID);
    r.dirty_count = 0; assert(!pw_d3d9_device_request_encode(wire, sizeof(wire), &n, &r));
    wire[32] = 1; assert(pw_d3d9_device_request_decode(&out, wire, n)); wire[32] = 0;
    wire[48] = 1; assert(pw_d3d9_device_request_decode(&out, wire, n)); wire[48] = 0;
    wire[68] = 1; assert(pw_d3d9_device_request_decode(&out, wire, n)); wire[68] = 0;
    wire[84] = 1; assert(pw_d3d9_device_request_decode(&out, wire, n)); wire[84] = 0;
    r.present_fields = 8; assert(pw_d3d9_device_request_encode(wire, sizeof(wire), &n, &r) == PW_D3D9_DEVICE_INVALID);
    r.operation = 4; assert(pw_d3d9_device_request_encode(wire, sizeof(wire), &n, &r) == PW_D3D9_DEVICE_UNSUPPORTED);

    parameters(&reply.parameters); reply.operation = PW_D3D9_DEVICE_CREATE;
    reply.object = (struct pw_d3d9_object_ref){18, 19}; reply_roundtrip(&reply, 88);
    reply.hresult = 1; reply_roundtrip(&reply, 88); /* Preserve successful nonzero HRESULT. */
    reply.hresult = UINT32_C(0x8876086c);
    assert(pw_d3d9_device_reply_encode(wire, sizeof(wire), &n, &reply) == PW_D3D9_DEVICE_INVALID);
    reply.object = (struct pw_d3d9_object_ref){0}; reply_roundtrip(&reply, 88);
    assert(!pw_d3d9_device_reply_encode(wire, sizeof(wire), &n, &reply));
    put32(wire + 16, 1); assert(pw_d3d9_device_reply_decode(&decoded, wire, n));
    reply.hresult = 0; assert(pw_d3d9_device_reply_encode(wire, sizeof(wire), &n, &reply) == PW_D3D9_DEVICE_INVALID);
    reply.operation = PW_D3D9_DEVICE_RESET; reply_roundtrip(&reply, 80);
    reply.hresult = UINT32_C(0x88760868); reply_roundtrip(&reply, 80);
    reply.operation = PW_D3D9_DEVICE_PRESENT; reply_roundtrip(&reply, 16);
    reply.hresult = 0; reply_roundtrip(&reply, 16);
    assert(pw_d3d9_device_reply_decode(NULL, wire, 16));
    assert(pw_d3d9_device_request_decode(&out, NULL, 16));
    assert(pw_d3d9_device_request_encode(wire, sizeof(wire), NULL, &r));
    puts("D3D9 device wire: PASS");
    return 0;
}
