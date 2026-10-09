/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_d3d9_device_wire.h"
#include <string.h>

static uint32_t get32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void put32(unsigned char *p, uint32_t n)
{
    unsigned i;
    for (i = 0; i < 4; ++i) p[i] = (unsigned char)(n >> (8 * i));
}
static int valid_operation(uint32_t op)
{
    return op >= PW_D3D9_DEVICE_CREATE && op <= PW_D3D9_DEVICE_PRESENT;
}
static int valid_window(struct pw_d3d9_window_id w)
{
    return (!w.epoch && !w.id && !w.generation) || (w.epoch && w.id && w.generation);
}
static void put_window(unsigned char *p, struct pw_d3d9_window_id w)
{
    put32(p, w.epoch); put32(p + 4, w.id); put32(p + 8, w.generation);
}
static struct pw_d3d9_window_id get_window(const unsigned char *p)
{
    struct pw_d3d9_window_id w = {get32(p), get32(p + 4), get32(p + 8)};
    return w;
}
static int valid_parameters(const struct pw_d3d9_present_parameters *p)
{
    return p->windowed <= 1 && p->auto_depth_stencil <= 1 && valid_window(p->window);
}
static void put_parameters(unsigned char *p, const struct pw_d3d9_present_parameters *v)
{
    unsigned i = 0;
#define PW_D3D9_PP_PUT(name, native) put32(p + 4 * i++, v->name);
    PW_D3D9_PRESENT_FIELDS(PW_D3D9_PP_PUT)
#undef PW_D3D9_PP_PUT
    put_window(p + 52, v->window);
}
static void get_parameters(struct pw_d3d9_present_parameters *v, const unsigned char *p)
{
    unsigned i = 0;
#define PW_D3D9_PP_GET(name, native) v->name = get32(p + 4 * i++);
    PW_D3D9_PRESENT_FIELDS(PW_D3D9_PP_GET)
#undef PW_D3D9_PP_GET
    v->window = get_window(p + 52);
}
static void put_rect(unsigned char *p, const struct pw_d3d9_rect *r)
{
    put32(p, (uint32_t)r->left); put32(p + 4, (uint32_t)r->top);
    put32(p + 8, (uint32_t)r->right); put32(p + 12, (uint32_t)r->bottom);
}
static void get_rect(struct pw_d3d9_rect *r, const unsigned char *p)
{
    uint32_t v;
    v = get32(p); memcpy(&r->left, &v, 4);
    v = get32(p + 4); memcpy(&r->top, &v, 4);
    v = get32(p + 8); memcpy(&r->right, &v, 4);
    v = get32(p + 12); memcpy(&r->bottom, &v, 4);
}
static int zero_bytes(const unsigned char *p, size_t n)
{
    size_t i;
    for (i = 0; i < n; ++i) if (p[i]) return 0;
    return 1;
}
int pw_d3d9_device_request_encode(void *wire, size_t capacity, size_t *written,
                                  const struct pw_d3d9_device_request *r)
{
    unsigned char tmp[PW_D3D9_DEVICE_MAX_REQUEST] = {0};
    size_t bytes;
    unsigned i;
    if (!written || !r) return PW_D3D9_DEVICE_INVALID;
    *written = 0;
    if (!valid_operation(r->operation)) return PW_D3D9_DEVICE_UNSUPPORTED;
    if (r->operation == PW_D3D9_DEVICE_PRESENT)
    {
        if (r->present_fields & ~7u || !valid_window(r->override_window) ||
            (!(r->present_fields & PW_D3D9_PRESENT_DIRTY) && r->dirty_count)) return PW_D3D9_DEVICE_INVALID;
        if (r->dirty_count > PW_D3D9_DEVICE_MAX_DIRTY_RECTS) return PW_D3D9_DEVICE_UNSUPPORTED;
        bytes = 88 + 16 * r->dirty_count;
        put32(tmp + 16, r->present_fields); put_window(tmp + 20, r->override_window);
        if (r->present_fields & PW_D3D9_PRESENT_SOURCE) put_rect(tmp + 32, &r->source);
        if (r->present_fields & PW_D3D9_PRESENT_DESTINATION) put_rect(tmp + 48, &r->destination);
        put32(tmp + 64, r->dirty_count);
        if (r->present_fields & PW_D3D9_PRESENT_DIRTY) put_rect(tmp + 68, &r->dirty_bounds);
        for (i = 0; i < r->dirty_count; ++i) put_rect(tmp + 88 + 16 * i, &r->dirty[i]);
    }
    else
    {
        if (!valid_parameters(&r->parameters)) return PW_D3D9_DEVICE_INVALID;
        if (r->operation == PW_D3D9_DEVICE_CREATE)
        {
            if (!valid_window(r->focus_window)) return PW_D3D9_DEVICE_INVALID;
            bytes = 104;
            put32(tmp + 16, r->adapter); put32(tmp + 20, r->device_type); put32(tmp + 24, r->behavior_flags);
            put_window(tmp + 28, r->focus_window); put_parameters(tmp + 40, &r->parameters);
        }
        else { bytes = 80; put_parameters(tmp + 16, &r->parameters); }
    }
    *written = bytes;
    if (capacity < bytes) return PW_D3D9_DEVICE_SMALL;
    if (!wire) return PW_D3D9_DEVICE_INVALID;
    put32(tmp, PW_D3D9_DEVICE_VERSION); put32(tmp + 4, r->operation); put32(tmp + 8, (uint32_t)bytes - 16);
    memcpy(wire, tmp, bytes);
    return PW_D3D9_DEVICE_OK;
}
int pw_d3d9_device_request_decode(struct pw_d3d9_device_request *out, const void *wire, size_t bytes)
{
    struct pw_d3d9_device_request r = {0};
    const unsigned char *p = wire;
    unsigned i;
    if (!out || !wire || bytes < 16 || bytes > PW_D3D9_DEVICE_MAX_REQUEST ||
        get32(p) != PW_D3D9_DEVICE_VERSION || get32(p + 12) || get32(p + 8) != bytes - 16)
        return PW_D3D9_DEVICE_INVALID;
    r.operation = get32(p + 4);
    if (!valid_operation(r.operation)) return PW_D3D9_DEVICE_UNSUPPORTED;
    if (r.operation == PW_D3D9_DEVICE_PRESENT)
    {
        if (bytes < 88) return PW_D3D9_DEVICE_INVALID;
        r.present_fields = get32(p + 16); r.override_window = get_window(p + 20); r.dirty_count = get32(p + 64);
        if (r.present_fields & ~7u || !valid_window(r.override_window) || get32(p + 84) ||
            r.dirty_count > PW_D3D9_DEVICE_MAX_DIRTY_RECTS || bytes != 88 + 16 * r.dirty_count)
            return PW_D3D9_DEVICE_INVALID;
        if (!(r.present_fields & PW_D3D9_PRESENT_SOURCE) && !zero_bytes(p + 32, 16)) return PW_D3D9_DEVICE_INVALID;
        if (!(r.present_fields & PW_D3D9_PRESENT_DESTINATION) && !zero_bytes(p + 48, 16)) return PW_D3D9_DEVICE_INVALID;
        if (!(r.present_fields & PW_D3D9_PRESENT_DIRTY) && (r.dirty_count || !zero_bytes(p + 68, 16)))
            return PW_D3D9_DEVICE_INVALID;
        get_rect(&r.source, p + 32); get_rect(&r.destination, p + 48); get_rect(&r.dirty_bounds, p + 68);
        for (i = 0; i < r.dirty_count; ++i) get_rect(&r.dirty[i], p + 88 + 16 * i);
    }
    else
    {
        if (r.operation == PW_D3D9_DEVICE_CREATE)
        {
            if (bytes != 104) return PW_D3D9_DEVICE_INVALID;
            r.adapter = get32(p + 16); r.device_type = get32(p + 20); r.behavior_flags = get32(p + 24);
            r.focus_window = get_window(p + 28); get_parameters(&r.parameters, p + 40);
            if (!valid_window(r.focus_window)) return PW_D3D9_DEVICE_INVALID;
        }
        else
        {
            if (bytes != 80) return PW_D3D9_DEVICE_INVALID;
            get_parameters(&r.parameters, p + 16);
        }
        if (!valid_parameters(&r.parameters)) return PW_D3D9_DEVICE_INVALID;
    }
    *out = r;
    return PW_D3D9_DEVICE_OK;
}
static int valid_reply(const struct pw_d3d9_device_reply *r)
{
    if (r->operation != PW_D3D9_DEVICE_PRESENT && !valid_parameters(&r->parameters)) return 0;
    if (r->operation == PW_D3D9_DEVICE_CREATE && !(r->hresult & UINT32_C(0x80000000)))
        return r->object.id && r->object.generation;
    return !r->object.id && !r->object.generation;
}
int pw_d3d9_device_reply_encode(void *wire, size_t capacity, size_t *written,
                                const struct pw_d3d9_device_reply *r)
{
    unsigned char tmp[PW_D3D9_DEVICE_MAX_REPLY] = {0};
    size_t bytes;
    if (!written || !r) return PW_D3D9_DEVICE_INVALID;
    *written = 0;
    if (!valid_operation(r->operation)) return PW_D3D9_DEVICE_UNSUPPORTED;
    if (!valid_reply(r)) return PW_D3D9_DEVICE_INVALID;
    bytes = r->operation == PW_D3D9_DEVICE_CREATE ? 88 : r->operation == PW_D3D9_DEVICE_RESET ? 80 : 16;
    *written = bytes;
    if (capacity < bytes) return PW_D3D9_DEVICE_SMALL;
    if (!wire) return PW_D3D9_DEVICE_INVALID;
    put32(tmp, PW_D3D9_DEVICE_VERSION); put32(tmp + 4, r->operation); put32(tmp + 8, r->hresult);
    put32(tmp + 12, (uint32_t)bytes - 16);
    if (r->operation == PW_D3D9_DEVICE_CREATE)
    {
        put32(tmp + 16, r->object.id); put32(tmp + 20, r->object.generation); put_parameters(tmp + 24, &r->parameters);
    }
    else if (r->operation == PW_D3D9_DEVICE_RESET) put_parameters(tmp + 16, &r->parameters);
    memcpy(wire, tmp, bytes);
    return PW_D3D9_DEVICE_OK;
}
int pw_d3d9_device_reply_decode(struct pw_d3d9_device_reply *out, const void *wire, size_t bytes)
{
    struct pw_d3d9_device_reply r = {0};
    const unsigned char *p = wire;
    size_t expected;
    if (!out || !wire || bytes < 16 || get32(p) != PW_D3D9_DEVICE_VERSION || get32(p + 12) != bytes - 16)
        return PW_D3D9_DEVICE_INVALID;
    r.operation = get32(p + 4); r.hresult = get32(p + 8);
    if (!valid_operation(r.operation)) return PW_D3D9_DEVICE_UNSUPPORTED;
    expected = r.operation == PW_D3D9_DEVICE_CREATE ? 88 : r.operation == PW_D3D9_DEVICE_RESET ? 80 : 16;
    if (bytes != expected) return PW_D3D9_DEVICE_INVALID;
    if (r.operation == PW_D3D9_DEVICE_CREATE)
    {
        r.object.id = get32(p + 16); r.object.generation = get32(p + 20); get_parameters(&r.parameters, p + 24);
    }
    else if (r.operation == PW_D3D9_DEVICE_RESET) get_parameters(&r.parameters, p + 16);
    if (!valid_reply(&r)) return PW_D3D9_DEVICE_INVALID;
    *out = r;
    return PW_D3D9_DEVICE_OK;
}
