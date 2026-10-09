/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_d3d9_command_wire.h"
#include <string.h>

static const struct pw_d3d9_command_schema schemas[] = {
#define SCHEMA(slot, name, words, shape, objects, booleans) {slot, words, PW_D3D9_DATA_##shape, objects, booleans},
    PW_D3D9_COMMAND_METHODS(SCHEMA)
#undef SCHEMA
};
const struct pw_d3d9_command_schema *pw_d3d9_command_schema(uint32_t method)
{
    unsigned i;
    for (i = 0; i < sizeof(schemas) / sizeof(schemas[0]); ++i)
        if (schemas[i].method == method) return &schemas[i];
    return NULL;
}
static uint32_t get32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void put32(unsigned char *p, uint32_t value)
{
    unsigned i;
    for (i = 0; i < 4; ++i) p[i] = (unsigned char)(value >> (8 * i));
}
int pw_d3d9_command_data_bytes(uint32_t method, const uint32_t args[PW_D3D9_COMMAND_WORDS], size_t *bytes)
{
    const struct pw_d3d9_command_schema *s = pw_d3d9_command_schema(method);
    uint64_t n = 0;
    unsigned i;
    if (!bytes || !args) return PW_D3D9_COMMAND_INVALID;
    *bytes = 0;
    if (!s) return PW_D3D9_COMMAND_UNSUPPORTED;
    for (i = 0; i < s->words; ++i)
    {
        if ((s->boolean_words & (1u << i)) && args[i] > 1) return PW_D3D9_COMMAND_INVALID;
        if ((s->object_words & (1u << i)) && (!!args[i] != !!args[i + 1])) return PW_D3D9_COMMAND_INVALID;
    }
    switch (s->shape)
    {
        case PW_D3D9_DATA_NONE: break;
        case PW_D3D9_DATA_MATRIX: n = 64; break;
        case PW_D3D9_DATA_VIEWPORT: n = 24; break;
        case PW_D3D9_DATA_MATERIAL: n = 68; break;
        case PW_D3D9_DATA_LIGHT: n = 104; break;
        case PW_D3D9_DATA_PLANE: case PW_D3D9_DATA_RECT: n = 16; break;
        case PW_D3D9_DATA_CLIP: n = 8; break;
        case PW_D3D9_DATA_PALETTE: n = 1024; break;
        case PW_D3D9_DATA_VECTOR4: n = (uint64_t)args[1] * 16; break;
        case PW_D3D9_DATA_BOOL: n = (uint64_t)args[1] * 4; break;
        case PW_D3D9_DATA_CLEAR: n = (uint64_t)args[0] * 16; break;
        default: return PW_D3D9_COMMAND_UNSUPPORTED;
    }
    if (n > PW_D3D9_COMMAND_DATA) return PW_D3D9_COMMAND_UNSUPPORTED;
    *bytes = (size_t)n;
    return PW_D3D9_COMMAND_OK;
}
static int valid_payload(const struct pw_d3d9_command_schema *s,
                         const unsigned char *data, size_t bytes, int wire)
{
    if (s->shape == PW_D3D9_DATA_BOOL)
        for (size_t i = 0; i < bytes; i += 4) {
            uint32_t value;
            if (wire) value = get32(data + i);
            else memcpy(&value, data + i, sizeof(value));
            if (value > 1) return 0;
        }
    return 1;
}
static int valid_data(const struct pw_d3d9_command_schema *s, const struct pw_d3d9_command *c)
{ return valid_payload(s, c->data.bytes, c->data_bytes, 0); }
int pw_d3d9_command_encode(void *wire, size_t capacity, size_t *written, const struct pw_d3d9_command *c)
{
    unsigned char tmp[PW_D3D9_COMMAND_MAX] = {0};
    const struct pw_d3d9_command_schema *s;
    size_t bytes, total;
    unsigned i;
    int result;
    if (!c || !written) return PW_D3D9_COMMAND_INVALID;
    *written = 0;
    result = pw_d3d9_command_data_bytes(c->method, c->args, &bytes);
    if (result) return result;
    s = pw_d3d9_command_schema(c->method);
    if (bytes != c->data_bytes || !valid_data(s, c)) return PW_D3D9_COMMAND_INVALID;
    total = 16 + 4 * s->words + bytes;
    *written = total;
    if (capacity < total) return PW_D3D9_COMMAND_SMALL;
    if (!wire) return PW_D3D9_COMMAND_INVALID;
    put32(tmp, PW_D3D9_COMMAND_VERSION); put32(tmp + 4, c->method);
    put32(tmp + 8, s->words); put32(tmp + 12, c->data_bytes);
    for (i = 0; i < s->words; ++i) put32(tmp + 16 + 4 * i, c->args[i]);
    if (s->shape == PW_D3D9_DATA_PALETTE) memcpy(tmp + 16 + 4 * s->words, c->data.bytes, bytes);
    else for (i = 0; i < bytes / 4; ++i) put32(tmp + 16 + 4 * s->words + 4 * i, c->data.words[i]);
    memcpy(wire, tmp, total);
    return PW_D3D9_COMMAND_OK;
}
/* Only metadata is needed by structural batch scans. Keep the public decoder's
 * error precedence, including unsupported methods/counts, in this shared parser. */
struct command_header {
    uint32_t method, data_bytes, args[PW_D3D9_COMMAND_WORDS];
    const struct pw_d3d9_command_schema *schema;
};
static int parse_header(struct command_header *h, const void *wire, size_t length)
{
    const unsigned char *p = wire;
    size_t bytes;
    int result;
    if (!wire || length < 16 || length > PW_D3D9_COMMAND_MAX || get32(p) != PW_D3D9_COMMAND_VERSION)
        return PW_D3D9_COMMAND_INVALID;
    memset(h, 0, sizeof(*h));
    h->method = get32(p + 4); h->data_bytes = get32(p + 12);
    h->schema = pw_d3d9_command_schema(h->method);
    if (!h->schema) return PW_D3D9_COMMAND_UNSUPPORTED;
    if (get32(p + 8) != h->schema->words || h->data_bytes > PW_D3D9_COMMAND_DATA ||
        length != 16 + 4 * h->schema->words + h->data_bytes) return PW_D3D9_COMMAND_INVALID;
    for (unsigned i = 0; i < h->schema->words; ++i) h->args[i] = get32(p + 16 + 4 * i);
    result = pw_d3d9_command_data_bytes(h->method, h->args, &bytes);
    if (result) return result;
    return bytes == h->data_bytes ? PW_D3D9_COMMAND_OK : PW_D3D9_COMMAND_INVALID;
}
int pw_d3d9_command_validate(const void *wire, size_t length)
{
    struct command_header h;
    int result = parse_header(&h, wire, length);
    if (result) return result;
    const unsigned char *payload = (const unsigned char *)wire + 16 + 4 * h.schema->words;
    return valid_payload(h.schema, payload, h.data_bytes, 1) ? PW_D3D9_COMMAND_OK : PW_D3D9_COMMAND_INVALID;
}
int pw_d3d9_command_decode(struct pw_d3d9_command *out, const void *wire, size_t length)
{
    struct command_header h;
    struct pw_d3d9_command c = {0};
    const unsigned char *p = wire;
    int result;
    if (!out) return PW_D3D9_COMMAND_INVALID;
    result = parse_header(&h, wire, length);
    if (result) return result;
    c.method = h.method; c.data_bytes = h.data_bytes;
    memcpy(c.args, h.args, sizeof(c.args));
    p += 16 + 4 * h.schema->words;
    if (h.schema->shape == PW_D3D9_DATA_PALETTE) memcpy(c.data.bytes, p, h.data_bytes);
    else for (unsigned i = 0; i < h.data_bytes / 4; ++i) c.data.words[i] = get32(p + 4 * i);
    /* Validate the owned payload, not the source a second time. This also keeps
     * overlapping input/output and unchanged output on failure intact. */
    if (!valid_data(h.schema, &c)) return PW_D3D9_COMMAND_INVALID;
    *out = c;
    return PW_D3D9_COMMAND_OK;
}
int pw_d3d9_command_reply_encode(void *wire, size_t capacity, size_t *written, uint32_t method, uint32_t hresult)
{
    unsigned char tmp[16] = {0};
    if (!written) return PW_D3D9_COMMAND_INVALID;
    *written = 0;
    if (!pw_d3d9_command_schema(method)) return PW_D3D9_COMMAND_UNSUPPORTED;
    *written = 16;
    if (capacity < 16) return PW_D3D9_COMMAND_SMALL;
    if (!wire) return PW_D3D9_COMMAND_INVALID;
    put32(tmp, PW_D3D9_COMMAND_VERSION); put32(tmp + 4, method); put32(tmp + 8, hresult);
    memcpy(wire, tmp, 16);
    return PW_D3D9_COMMAND_OK;
}
int pw_d3d9_command_reply_decode(uint32_t *method, uint32_t *hresult, const void *wire, size_t length)
{
    const unsigned char *p = wire;
    if (!method || !hresult || !wire || length != 16 || get32(p) != PW_D3D9_COMMAND_VERSION || get32(p + 12))
        return PW_D3D9_COMMAND_INVALID;
    if (!pw_d3d9_command_schema(get32(p + 4))) return PW_D3D9_COMMAND_UNSUPPORTED;
    *method = get32(p + 4); *hresult = get32(p + 8);
    return PW_D3D9_COMMAND_OK;
}
