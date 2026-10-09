/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Drive a real DXVK device and the transform shadow with the same calls and
 * outcomes. Every value the shadow claims to know must equal the backend's
 * GetTransform bytes; the shadow may only ever be less informed. */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "pw_d3d9_transform_shadow.h"
#define BLOCKS 8
static IDirect3DDevice9 *device;
static D3DPRESENT_PARAMETERS pp;
static struct pw_d3d9_transform_shadow shadow;
static struct { IDirect3DStateBlock9 *native; struct pw_d3d9_transform_block evidence; } blocks[BLOCKS];
static unsigned long long max_known, compared, hits, mismatches, operations, resets, reset_ok;
static uint32_t rng = 0x12345678u;
static uint32_t next(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static uint32_t state_at(unsigned slot)
{
    if (slot == 0) return D3DTS_VIEW;
    if (slot == 1) return D3DTS_PROJECTION;
    if (slot < 10) return D3DTS_TEXTURE0 + (slot - 2);
    return 256 + (slot - 10);
}
static uint32_t pick_state(void)
{
    static const uint32_t hot[] = {2, 3, 16, 17, 23, 256, 257, 300, 511};
    return next() % 4 ? hot[next() % 9] : state_at(next() % PW_D3D9_TRANSFORM_SLOTS);
}
static void pick_matrix(D3DMATRIX *m)
{
    uint32_t words[16];
    for (unsigned i = 0; i < 16; i++) {
        switch (next() % 8) {
        case 0: words[i] = 0x7fc00001u; break;           /* quiet NaN payload */
        case 1: words[i] = 0x7f800001u; break;           /* signalling NaN */
        case 2: words[i] = 0x80000000u; break;           /* -0 */
        case 3: words[i] = 0x00000001u; break;           /* denormal */
        case 4: words[i] = 0xff800000u; break;           /* -inf */
        default: words[i] = next(); break;
        }
    }
    memcpy(m, words, sizeof(words));
}
/* Compare every value the shadow claims; misses are allowed and not counted. */
static void sweep(const char *step)
{
    unsigned long long known = 0;
    for (unsigned slot = 0; slot < PW_D3D9_TRANSFORM_SLOTS; slot++) {
        uint32_t state = state_at(slot);
        unsigned char local[64];
        D3DMATRIX native;
        if (!pw_d3d9_transform_lookup(&shadow, state, local)) continue;
        memset(&native, 0xcd, sizeof(native));
        HRESULT hr = IDirect3DDevice9_GetTransform(device, state, &native);
        compared++; known++;
        if (hr != D3D_OK || memcmp(local, &native, 64)) {
            if (mismatches++ < 8) printf("PW_TRANSFORM_MISMATCH step=%s op=%llu state=%u hr=%08lx\n", step, operations, state, hr);
        }
    }
    if (known > max_known) max_known = known;
}
static int free_block(void)
{
    for (unsigned i = 0; i < BLOCKS; i++) if (!blocks[i].native) return (int)i;
    return -1;
}
static void release_block(unsigned i)
{
    if (!blocks[i].native) return;
    IDirect3DStateBlock9_Release(blocks[i].native);
    blocks[i].native = NULL;
    pw_d3d9_transform_block_free(&blocks[i].evidence);
}
static void step(void)
{
    D3DMATRIX m;
    HRESULT hr;
    uint32_t state = pick_state();
    unsigned op = next() % 100;
    int b = (int)(next() % BLOCKS);
    operations++;
    if (op < 40) {
        pick_matrix(&m);
        int null = next() % 16 == 0;
        hr = IDirect3DDevice9_SetTransform(device, state, null ? NULL : &m);
        pw_d3d9_transform_set(&shadow, state, null ? NULL : &m, (uint32_t)hr);
        sweep("set");
    } else if (op < 48) {
        pick_matrix(&m);
        hr = IDirect3DDevice9_MultiplyTransform(device, state, &m);
        pw_d3d9_transform_multiply(&shadow, state);
        sweep("multiply");
    } else if (op < 60) {
        hr = IDirect3DDevice9_GetTransform(device, state, &m);
        unsigned char local[64];
        if (pw_d3d9_transform_lookup(&shadow, state, local)) {
            hits++;compared++;
            if (hr != D3D_OK || memcmp(local, &m, 64)) {
                if (mismatches++ < 8) printf("PW_TRANSFORM_MISMATCH step=get op=%llu state=%u hr=%08lx\n", operations, state, hr);
            }
        }
        pw_d3d9_transform_observe(&shadow, state, &m, (uint32_t)hr);
        sweep("observe");
    } else if (op < 66) {
        hr = IDirect3DDevice9_BeginStateBlock(device);
        pw_d3d9_transform_begin(&shadow, (uint32_t)hr);
        sweep("begin");
    } else if (op < 72) {
        int slot = free_block();
        IDirect3DStateBlock9 *sb = NULL;
        struct pw_d3d9_transform_block scratch = {0}, *evidence = slot >= 0 ? &blocks[slot].evidence : &scratch;
        pw_d3d9_transform_block_prepare(evidence);
        hr = IDirect3DDevice9_EndStateBlock(device, &sb);
        pw_d3d9_transform_end(&shadow, evidence, (uint32_t)hr);
        if (sb && slot >= 0) blocks[slot].native = sb;
        else { if (sb) IDirect3DStateBlock9_Release(sb); pw_d3d9_transform_block_free(evidence); }
        sweep("end");
    } else if (op < 78) {
        static const D3DSTATEBLOCKTYPE types[] = {D3DSBT_ALL, D3DSBT_PIXELSTATE, D3DSBT_VERTEXSTATE};
        D3DSTATEBLOCKTYPE type = types[next() % 3];
        int slot = free_block();
        IDirect3DStateBlock9 *sb = NULL;
        if (slot < 0) { release_block((unsigned)b); slot = b; }
        pw_d3d9_transform_block_prepare(&blocks[slot].evidence);
        hr = IDirect3DDevice9_CreateStateBlock(device, type, &sb);
        pw_d3d9_transform_create(&shadow, &blocks[slot].evidence, (uint32_t)type, (uint32_t)hr);
        if (sb) blocks[slot].native = sb; else pw_d3d9_transform_block_free(&blocks[slot].evidence);
        sweep("create");
    } else if (op < 86) {
        if (!blocks[b].native) return;
        hr = IDirect3DStateBlock9_Capture(blocks[b].native);
        pw_d3d9_transform_capture(&shadow, &blocks[b].evidence, (uint32_t)hr);
        sweep("capture");
    } else if (op < 96) {
        if (!blocks[b].native) return;
        hr = IDirect3DStateBlock9_Apply(blocks[b].native);
        pw_d3d9_transform_apply(&shadow, &blocks[b].evidence, (uint32_t)hr);
        sweep("apply");
    } else if (op < 98) {
        release_block((unsigned)b);
        sweep("release");
    } else if (next() % 8) {
        release_block((unsigned)b); /* keep Reset rare so restored values accumulate */
        sweep("release");
    } else {
        /* Reset fails while state blocks are alive; release them sometimes. */
        if (next() % 2) for (unsigned i = 0; i < BLOCKS; i++) release_block(i);
        D3DPRESENT_PARAMETERS copy = pp;
        hr = IDirect3DDevice9_Reset(device, &copy);
        resets++; reset_ok += hr == D3D_OK;
        pw_d3d9_transform_invalidate(&shadow);
        sweep("reset");
    }
}
int main(void)
{
    WCHAR path[260], text[16];
    DWORD flags = D3DCREATE_HARDWARE_VERTEXPROCESSING;
    unsigned steps = 6000;
    if (GetEnvironmentVariableW(L"PW_TRANSFORM_BEHAVIOR", text, 16)) flags = wcstoul(text, NULL, 0);
    if (GetEnvironmentVariableW(L"PW_TRANSFORM_SEED", text, 16)) rng = wcstoul(text, NULL, 0) | 1u;
    if (!GetEnvironmentVariableW(L"PW_TRANSFORM_BACKEND", path, 260)) return 2;
    HMODULE backend = LoadLibraryW(path);
    IDirect3D9 *(WINAPI *create)(UINT) = backend ? (void *)GetProcAddress(backend, "Direct3DCreate9") : NULL;
    IDirect3D9 *d3d = create ? create(D3D_SDK_VERSION) : NULL;
    if (!d3d) { printf("PW_TRANSFORM_PE backend=missing\n"); return 3; }
    HWND window = CreateWindowExW(0, L"static", L"transform shadow", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, NULL, NULL, NULL, NULL);
    pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.BackBufferWidth = 64; pp.BackBufferHeight = 64;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8; pp.hDeviceWindow = window;
    HRESULT hr = IDirect3D9_CreateDevice(d3d, 0, D3DDEVTYPE_HAL, window, flags, &pp, &device);
    if (FAILED(hr)) { printf("PW_TRANSFORM_PE create=%08lx\n", hr); return 4; }
    pw_d3d9_transform_init(&shadow);
    /* Deterministic edges first: NULL identity, ALL/recorded restore, refused
     * Capture/Apply while recording, Reset with and without live blocks. */
    D3DMATRIX m; IDirect3DStateBlock9 *all = NULL, *recorded = NULL;
    struct pw_d3d9_transform_block all_e = {0}, rec_e = {0};
    unsigned edge = 0;
    pick_matrix(&m);
    hr = IDirect3DDevice9_SetTransform(device, D3DTS_VIEW, NULL); pw_d3d9_transform_set(&shadow, D3DTS_VIEW, NULL, (uint32_t)hr);
    hr = IDirect3DDevice9_SetTransform(device, 511, &m); pw_d3d9_transform_set(&shadow, 511, &m, (uint32_t)hr);
    edge += pw_d3d9_transform_lookup(&shadow, D3DTS_VIEW, &m) && pw_d3d9_transform_lookup(&shadow, 511, &m);
    sweep("edge-set");
    pw_d3d9_transform_block_prepare(&all_e);
    hr = IDirect3DDevice9_CreateStateBlock(device, D3DSBT_ALL, &all); pw_d3d9_transform_create(&shadow, &all_e, D3DSBT_ALL, (uint32_t)hr);
    hr = IDirect3DDevice9_BeginStateBlock(device); pw_d3d9_transform_begin(&shadow, (uint32_t)hr);
    pick_matrix(&m);
    hr = IDirect3DDevice9_SetTransform(device, D3DTS_PROJECTION, &m); pw_d3d9_transform_set(&shadow, D3DTS_PROJECTION, &m, (uint32_t)hr);
    hr = IDirect3DStateBlock9_Apply(all); edge += hr == D3DERR_INVALIDCALL; pw_d3d9_transform_apply(&shadow, &all_e, (uint32_t)hr);
    hr = IDirect3DStateBlock9_Capture(all); edge += hr == D3DERR_INVALIDCALL; pw_d3d9_transform_capture(&shadow, &all_e, (uint32_t)hr);
    sweep("edge-recording");
    pw_d3d9_transform_block_prepare(&rec_e);
    hr = IDirect3DDevice9_EndStateBlock(device, &recorded); pw_d3d9_transform_end(&shadow, &rec_e, (uint32_t)hr);
    edge += hr == D3D_OK && rec_e.known;
    pick_matrix(&m);
    hr = IDirect3DDevice9_SetTransform(device, D3DTS_PROJECTION, &m); pw_d3d9_transform_set(&shadow, D3DTS_PROJECTION, &m, (uint32_t)hr);
    hr = IDirect3DStateBlock9_Apply(recorded); pw_d3d9_transform_apply(&shadow, &rec_e, (uint32_t)hr);
    edge += hr == D3D_OK && pw_d3d9_transform_lookup(&shadow, D3DTS_PROJECTION, &m);
    sweep("edge-recorded-apply");
    hr = IDirect3DStateBlock9_Apply(all); pw_d3d9_transform_apply(&shadow, &all_e, (uint32_t)hr);
    edge += hr == D3D_OK && pw_d3d9_transform_lookup(&shadow, D3DTS_VIEW, &m);
    sweep("edge-all-apply");
    D3DPRESENT_PARAMETERS copy = pp;
    hr = IDirect3DDevice9_Reset(device, &copy); pw_d3d9_transform_invalidate(&shadow);
    printf("PW_TRANSFORM_RESET live_blocks=1 hr=%08lx\n", hr);
    IDirect3DStateBlock9_Release(all); IDirect3DStateBlock9_Release(recorded);
    pw_d3d9_transform_block_free(&all_e); pw_d3d9_transform_block_free(&rec_e);
    copy = pp;
    hr = IDirect3DDevice9_Reset(device, &copy); pw_d3d9_transform_invalidate(&shadow);
    edge += hr == D3D_OK;
    printf("PW_TRANSFORM_RESET live_blocks=0 hr=%08lx\n", hr);
    for (unsigned i = 0; i < steps; i++) step();
    for (unsigned i = 0; i < BLOCKS; i++) release_block(i);
    unsigned known = 0;
    for (unsigned slot = 0; slot < PW_D3D9_TRANSFORM_SLOTS; slot++) known += pw_d3d9_transform_lookup(&shadow, state_at(slot), &m);
    printf("PW_TRANSFORM_PE behavior=%08lx seed_ops=%llu edges=%u compared=%llu hits=%llu resets=%llu reset_ok=%llu known_end=%u max_known=%llu mismatches=%llu\n",
           flags, operations, edge, compared, hits, resets, reset_ok, known, max_known, mismatches);
    IDirect3DDevice9_Release(device); IDirect3D9_Release(d3d); DestroyWindow(window);
    return mismatches || edge != 7 ? 1 : 0;
}
