/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "wine/ps5/pw_d3d9_command_wire.h"
#ifdef _WIN32
#include <windows.h>
#include <d3d9.h>
#define SLOT(slot, name, words, shape, objects, booleans) \
    _Static_assert(offsetof(IDirect3DDevice9Vtbl, name) == (slot) * sizeof(void *), "device method slot");
PW_D3D9_COMMAND_METHODS(SLOT)
#undef SLOT
_Static_assert(sizeof(D3DMATRIX) == 64, "matrix words");
_Static_assert(sizeof(D3DVIEWPORT9) == 24, "viewport words");
_Static_assert(sizeof(D3DMATERIAL9) == 68, "material words");
_Static_assert(sizeof(D3DLIGHT9) == 104, "light words");
_Static_assert(offsetof(D3DLIGHT9, Diffuse) == 4, "light diffuse");
_Static_assert(offsetof(D3DLIGHT9, Specular) == 20, "light specular");
_Static_assert(offsetof(D3DLIGHT9, Ambient) == 36, "light ambient");
_Static_assert(offsetof(D3DLIGHT9, Position) == 52, "light position");
_Static_assert(offsetof(D3DLIGHT9, Direction) == 64, "light direction");
_Static_assert(offsetof(D3DLIGHT9, Range) == 76, "light range");
_Static_assert(offsetof(D3DLIGHT9, Phi) == 100, "light phi");
_Static_assert(sizeof(D3DCLIPSTATUS9) == 8, "clip status");
_Static_assert(sizeof(RECT) == 16 && sizeof(D3DRECT) == 16, "rectangle words");
_Static_assert(sizeof(PALETTEENTRY) == 4, "palette bytes");
#endif
#define METHOD(slot, name, words, shape, objects, booleans) slot,
static const unsigned methods[] = {PW_D3D9_COMMAND_METHODS(METHOD)};
#undef METHOD
static void put32(unsigned char *p, uint32_t value)
{
    unsigned i;
    for (i = 0; i < 4; ++i) p[i] = (unsigned char)(value >> (8 * i));
}
/* Frozen pre-optimization decoder, kept independent of the new parser. */
static uint32_t get32(const unsigned char *p)
{ return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static int reference_decode(struct pw_d3d9_command *out, const void *wire, size_t length)
{
    struct pw_d3d9_command c = {0};
    const unsigned char *p = wire;
    const struct pw_d3d9_command_schema *schema;
    size_t bytes;
    int result;
    if (!out || !wire || length < 16 || length > PW_D3D9_COMMAND_MAX || get32(p) != PW_D3D9_COMMAND_VERSION)
        return PW_D3D9_COMMAND_INVALID;
    c.method = get32(p + 4); c.data_bytes = get32(p + 12);
    schema = pw_d3d9_command_schema(c.method);
    if (!schema) return PW_D3D9_COMMAND_UNSUPPORTED;
    if (get32(p + 8) != schema->words || c.data_bytes > PW_D3D9_COMMAND_DATA ||
        length != 16 + 4 * schema->words + c.data_bytes) return PW_D3D9_COMMAND_INVALID;
    for (unsigned i = 0; i < schema->words; ++i) c.args[i] = get32(p + 16 + 4 * i);
    result = pw_d3d9_command_data_bytes(c.method, c.args, &bytes);
    if (result) return result;
    if (bytes != c.data_bytes) return PW_D3D9_COMMAND_INVALID;
    if (schema->shape == PW_D3D9_DATA_PALETTE) memcpy(c.data.bytes, p + 16 + 4 * schema->words, bytes);
    else for (unsigned i = 0; i < bytes / 4; ++i) c.data.words[i] = get32(p + 16 + 4 * schema->words + 4 * i);
    if (schema->shape == PW_D3D9_DATA_BOOL)
        for (unsigned i = 0; i < bytes / 4; ++i) if (c.data.words[i] > 1) return PW_D3D9_COMMAND_INVALID;
    *out = c; return PW_D3D9_COMMAND_OK;
}
static unsigned comparisons;
static void equivalent(const void *wire, size_t bytes)
{
    struct pw_d3d9_command expected, actual, before;
    memset(&before, 0x5a, sizeof(before)); expected = actual = before;
    int status = reference_decode(&expected, wire, bytes);
    assert(pw_d3d9_command_validate(wire, bytes) == status);
    assert(pw_d3d9_command_decode(&actual, wire, bytes) == status);
    assert(!memcmp(&expected, &actual, sizeof(actual)));
    if (status) assert(!memcmp(&actual, &before, sizeof(actual)));
    ++comparisons;
}
static void validation_corpus(const unsigned char *wire, size_t bytes)
{
    unsigned char changed[PW_D3D9_COMMAND_MAX + 1];
    for (size_t n = 0; n <= bytes + 1; ++n) equivalent(wire, n);
    for (size_t n = 0; n < bytes; ++n) {
        memcpy(changed, wire, bytes); changed[n] ^= 0xff; equivalent(changed, bytes);
    }
    /* Decoder still owns/copies before writing an overlapping output. */
    union {struct pw_d3d9_command command; unsigned char bytes[PW_D3D9_COMMAND_MAX];} overlap;
    struct pw_d3d9_command expected;
    assert(!reference_decode(&expected, wire, bytes));
    memcpy(overlap.bytes, wire, bytes);
    assert(!pw_d3d9_command_decode(&overlap.command, overlap.bytes, bytes));
    assert(!memcmp(&overlap.command, &expected, sizeof(expected)));
}
static void exercise(unsigned method)
{
    const struct pw_d3d9_command_schema *s = pw_d3d9_command_schema(method);
    struct pw_d3d9_command c = {0}, out, marker;
    unsigned char wire[PW_D3D9_COMMAND_MAX + 1], again[PW_D3D9_COMMAND_MAX + 1];
    size_t data_bytes, bytes, n, i;
    uint32_t reply_method, hr;
    assert(s && s->words <= PW_D3D9_COMMAND_WORDS);
    c.method = method;
    for (i = 0; i < s->words; ++i) c.args[i] = (s->boolean_words & (1u << i)) ? 1 : 3;
    assert(!pw_d3d9_command_data_bytes(method, c.args, &data_bytes));
    c.data_bytes = (uint32_t)data_bytes;
    for (i = 0; i < data_bytes / 4; ++i) c.data.words[i] = s->shape == PW_D3D9_DATA_BOOL ? i % 2 : UINT32_C(0x81234500) + (uint32_t)i;
    memset(wire, 0xa5, sizeof(wire));
    assert(!pw_d3d9_command_encode(wire, sizeof(wire), &bytes, &c));
    assert(bytes == 16 + 4 * s->words + data_bytes && wire[bytes] == 0xa5);
    validation_corpus(wire, bytes);
    assert(!pw_d3d9_command_decode(&out, wire, bytes));
    assert(!pw_d3d9_command_encode(again, sizeof(again), &n, &out));
    assert(n == bytes && !memcmp(again, wire, bytes));
    memset(&marker, 0x5a, sizeof(marker));
    for (i = 0; i < bytes; ++i)
    {
        out = marker;
        assert(pw_d3d9_command_decode(&out, wire, i));
        assert(!memcmp(&out, &marker, sizeof(marker)));
    }
    assert(pw_d3d9_command_decode(&out, wire, bytes + 1));
    memset(again, 0x5a, sizeof(again));
    assert(pw_d3d9_command_encode(again, bytes - 1, &n, &c) == PW_D3D9_COMMAND_SMALL && n == bytes);
    for (i = 0; i < sizeof(again); ++i) assert(again[i] == 0x5a);
    put32(wire + 8, s->words + 1); assert(pw_d3d9_command_decode(&out, wire, bytes));
    for (i = 0; i < s->words; ++i)
    {
        if (s->object_words & (1u << i))
        {
            uint32_t saved = c.args[i]; c.args[i] = 0;
            assert(pw_d3d9_command_encode(wire, sizeof(wire), &n, &c) == PW_D3D9_COMMAND_INVALID);
            c.args[i + 1] = 0; assert(!pw_d3d9_command_encode(wire, sizeof(wire), &n, &c));
            c.args[i] = c.args[i + 1] = saved;
        }
        if (s->boolean_words & (1u << i))
        {
            c.args[i] = 2;
            assert(pw_d3d9_command_encode(wire, sizeof(wire), &n, &c) == PW_D3D9_COMMAND_INVALID);
            c.args[i] = 1;
        }
    }
    assert(!pw_d3d9_command_reply_encode(wire, sizeof(wire), &n, method, UINT32_C(0x8876086c)) && n == 16);
    assert(!pw_d3d9_command_reply_decode(&reply_method, &hr, wire, n));
    assert(reply_method == method && hr == UINT32_C(0x8876086c));
    for (i = 0; i < n; ++i)
    {
        reply_method = hr = 0xa5;
        assert(pw_d3d9_command_reply_decode(&reply_method, &hr, wire, i));
        assert(reply_method == 0xa5 && hr == 0xa5);
    }
    wire[12] = 1; assert(pw_d3d9_command_reply_decode(&reply_method, &hr, wire, n));
}
int main(void)
{
    struct pw_d3d9_command c = {0}, out;
    unsigned char wire[PW_D3D9_COMMAND_MAX];
    uint32_t args[PW_D3D9_COMMAND_WORDS] = {0};
    size_t bytes, n;
    unsigned i;
    for (i = 0; i < sizeof(methods) / sizeof(methods[0]); ++i) exercise(methods[i]);
    equivalent(NULL, 0); equivalent(NULL, 16);
    equivalent(wire, PW_D3D9_COMMAND_MAX + 1);
    /* Multiple simultaneous faults preserve the original status precedence. */
    memset(wire, 0, sizeof(wire)); put32(wire, PW_D3D9_COMMAND_VERSION);
    put32(wire + 4, 999); put32(wire + 8, UINT32_MAX);
    assert(pw_d3d9_command_validate(wire, 16) == PW_D3D9_COMMAND_UNSUPPORTED); equivalent(wire, 16);
    wire[0] = 0;
    assert(pw_d3d9_command_validate(wire, 16) == PW_D3D9_COMMAND_INVALID); equivalent(wire, 16);
    put32(wire, PW_D3D9_COMMAND_VERSION); put32(wire + 4, 94); put32(wire + 8, 2);
    put32(wire + 20, UINT32_MAX);
    assert(pw_d3d9_command_validate(wire, 24) == PW_D3D9_COMMAND_UNSUPPORTED); equivalent(wire, 24);
    put32(wire + 8, 3);
    assert(pw_d3d9_command_validate(wire, 24) == PW_D3D9_COMMAND_INVALID); equivalent(wire, 24);
    assert(!pw_d3d9_command_schema(83) && !pw_d3d9_command_schema(84));
    assert(pw_d3d9_command_data_bytes(83, args, &n) == PW_D3D9_COMMAND_UNSUPPORTED);
    assert(pw_d3d9_command_reply_encode(wire, sizeof(wire), &n, 0, 0) == PW_D3D9_COMMAND_UNSUPPORTED);
    args[1] = UINT32_MAX; assert(pw_d3d9_command_data_bytes(94, args, &n) == PW_D3D9_COMMAND_UNSUPPORTED);
    args[0] = UINT32_MAX; assert(pw_d3d9_command_data_bytes(43, args, &n) == PW_D3D9_COMMAND_UNSUPPORTED);
    c.method = 94; c.args[1] = 256; c.data_bytes = PW_D3D9_COMMAND_DATA;
    c.data.words[0] = UINT32_C(0x7fc12345); /* NaN payload is an integer bit pattern. */
    c.data.words[1] = UINT32_MAX;
    assert(!pw_d3d9_command_encode(wire, sizeof(wire), &bytes, &c));
    assert(bytes == 24 + 4096 && wire[24] == 0x45 && wire[27] == 0x7f);
    validation_corpus(wire, bytes);
    assert(!pw_d3d9_command_decode(&out, wire, bytes));
    assert(out.data.words[0] == c.data.words[0] && out.data.words[1] == UINT32_MAX);
    c.args[1] = 257; assert(pw_d3d9_command_encode(wire, sizeof(wire), &n, &c) == PW_D3D9_COMMAND_UNSUPPORTED);
    c.method = 98; c.args[1] = 1024; c.data.words[0] = 2;
    assert(pw_d3d9_command_encode(wire, sizeof(wire), &n, &c) == PW_D3D9_COMMAND_INVALID);
    memset(&c.data, 0, sizeof(c.data));
    assert(!pw_d3d9_command_encode(wire, sizeof(wire), &bytes, &c));
    validation_corpus(wire, bytes);
    wire[24] = 2; equivalent(wire, bytes); assert(pw_d3d9_command_decode(&out, wire, bytes));
    c.args[1] = 0; c.data_bytes = 0; assert(!pw_d3d9_command_encode(wire, sizeof(wire), &bytes, &c) && bytes == 24);
    c.method = 43; c.args[0] = 256; c.data_bytes = 4096;
    assert(!pw_d3d9_command_encode(wire, sizeof(wire), &bytes, &c));
    c.method = 71; c.args[0] = 0; c.data_bytes = 1024;
    for (i = 0; i < 1024; ++i) c.data.bytes[i] = (unsigned char)i;
    assert(!pw_d3d9_command_encode(wire, sizeof(wire), &bytes, &c));
    assert(!memcmp(wire + 20, c.data.bytes, 1024));
    assert(!pw_d3d9_command_decode(&out, wire, bytes));
    assert(!memcmp(out.data.bytes, c.data.bytes, 1024));
    assert(pw_d3d9_command_decode(NULL, wire, bytes));
    assert(pw_d3d9_command_data_bytes(43, NULL, &n));
    printf("D3D9 command wire: PASS methods=%u equivalence=%u\n", (unsigned)(sizeof(methods) / sizeof(methods[0])), comparisons);
    return 0;
}
