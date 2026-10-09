/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Drive the real production proxy with one seeded sequence of Transform,
 * state block, Reset and draw calls. Every GetTransform result is printed, so
 * runs with PW_D3D9_ASYNC=0 (each answer comes from DXVK) and =1 (known
 * answers come from the proxy) can be compared line by line. */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define BLOCKS 8
static IDirect3DDevice9 *device;
static D3DPRESENT_PARAMETERS pp;
static IDirect3DStateBlock9 *blocks[BLOCKS];
static uint32_t rng;
static unsigned get_count, step_no;
static uint32_t next(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static uint32_t pick_state(void)
{
    static const uint32_t hot[] = {2, 3, 16, 17, 23, 256, 257, 300, 511};
    if (next() % 4) return hot[next() % 9];
    uint32_t slot = next() % 266;
    return slot == 0 ? 2 : slot == 1 ? 3 : slot < 10 ? 16 + slot - 2 : 256 + slot - 10;
}
static void pick_matrix(D3DMATRIX *m)
{
    uint32_t w[16];
    for (unsigned i = 0; i < 16; i++) {
        switch (next() % 8) {
        case 0: w[i] = 0x7fc00001u; break; case 1: w[i] = 0x7f800001u; break;
        case 2: w[i] = 0x80000000u; break; case 3: w[i] = 1u; break;
        default: w[i] = next(); break;
        }
    }
    memcpy(m, w, sizeof(w));
}
static uint64_t fnv(const void *p, size_t n)
{
    const unsigned char *b = p; uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}
static void get(uint32_t state)
{
    D3DMATRIX m; memset(&m, 0xcd, sizeof(m));
    HRESULT hr = IDirect3DDevice9_GetTransform(device, state, &m);
    get_count++;
    printf("PW_TRANSFORM_GET step=%u state=%u hr=%08lx value=%016llx\n", step_no, state, hr, (unsigned long long)fnv(&m, sizeof(m)));
}
struct vertex { float x, y, z, w; DWORD c; };
static IDirect3DVertexBuffer9 *vb;
static IDirect3DIndexBuffer9 *ib;
/* GTA-like batch: queued sampler/render/constant setters around indexed draws. */
static void draw(void)
{
    IDirect3DDevice9_SetFVF(device, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    IDirect3DDevice9_SetStreamSource(device, 0, vb, 0, sizeof(struct vertex));
    IDirect3DDevice9_SetIndices(device, ib);
    IDirect3DDevice9_BeginScene(device);
    for (unsigned i = 0; i < 4; i++) {
        float c[4] = {(float)(next() % 7), 1, 0, 1};
        IDirect3DDevice9_SetSamplerState(device, i, D3DSAMP_ADDRESSU, 1 + next() % 3);
        IDirect3DDevice9_SetSamplerState(device, i, D3DSAMP_MAGFILTER, 1 + next() % 2);
        IDirect3DDevice9_SetRenderState(device, D3DRS_ALPHAREF, next() % 256);
        IDirect3DDevice9_SetVertexShaderConstantF(device, i, c, 1);
        IDirect3DDevice9_DrawIndexedPrimitive(device, D3DPT_TRIANGLELIST, 0, 0, 3, 0, 1);
        IDirect3DDevice9_DrawPrimitive(device, D3DPT_TRIANGLELIST, 0, 1);
    }
    IDirect3DDevice9_EndScene(device);
}
/* Render a known triangle into a private target and read it back. */
static void readback_with(const char *label,int up);
/* Setters issued while a state block is being recorded only record (the native
 * control shows SetStreamSource then leaves stream 0 unbound), so the
 * readback first ends any recording the random workload left open. */
static void readback(const char *label)
{
    IDirect3DStateBlock9 *sb = NULL;
    HRESULT hr = IDirect3DDevice9_EndStateBlock(device, &sb);
    if (sb) IDirect3DStateBlock9_Release(sb);
    /* A refused End leaves the proxy's recording knowledge unknown, which keeps
     * draws synchronous; an empty successful Begin/End pair re-establishes it. */
    HRESULT begin = IDirect3DDevice9_BeginStateBlock(device), end; sb = NULL;
    end = IDirect3DDevice9_EndStateBlock(device, &sb);
    if (sb) IDirect3DStateBlock9_Release(sb);
    printf("PW_TRANSFORM_READBACK_PREPARE label=%s end_recording=%08lx begin=%08lx end=%08lx\n", label, hr, begin, end);
    readback_with(label,1);readback_with(label,0);
}
static void readback_with(const char *label,int up)
{
    IDirect3DSurface9 *original = NULL, *target = NULL, *copy = NULL; D3DLOCKED_RECT lock; uint64_t h = 0; DWORD center = 0;
    HRESULT hr = IDirect3DDevice9_GetRenderTarget(device, 0, &original);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice9_CreateRenderTarget(device, 64, 64, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &target, NULL);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice9_CreateOffscreenPlainSurface(device, 64, 64, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &copy, NULL);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice9_SetRenderTarget(device, 0, target);
    if (SUCCEEDED(hr)) {
        IDirect3DDevice9_SetRenderState(device, D3DRS_LIGHTING, FALSE);
        IDirect3DDevice9_SetRenderState(device, D3DRS_CULLMODE, D3DCULL_NONE);
        IDirect3DDevice9_SetRenderState(device, D3DRS_ZENABLE, FALSE);
        IDirect3DDevice9_SetRenderState(device, D3DRS_ALPHATESTENABLE, FALSE);
        IDirect3DDevice9_SetRenderState(device, D3DRS_ALPHABLENDENABLE, FALSE);
        IDirect3DDevice9_SetVertexShader(device, NULL); IDirect3DDevice9_SetPixelShader(device, NULL);
        IDirect3DDevice9_SetTexture(device, 0, NULL);
        IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
        IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
        IDirect3DDevice9_SetTextureStageState(device, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        IDirect3DDevice9_SetRenderState(device, D3DRS_COLORWRITEENABLE, 0xf);
        IDirect3DDevice9_SetRenderState(device, D3DRS_STENCILENABLE, FALSE);
        IDirect3DDevice9_SetRenderState(device, D3DRS_SCISSORTESTENABLE, FALSE);
        IDirect3DDevice9_SetRenderState(device, D3DRS_FOGENABLE, FALSE);
        IDirect3DDevice9_SetRenderState(device, D3DRS_FILLMODE, D3DFILL_SOLID);
        IDirect3DDevice9_SetRenderState(device, D3DRS_CLIPPLANEENABLE, 0);
        {D3DVIEWPORT9 vp = {0, 0, 64, 64, 0, 1}; IDirect3DDevice9_SetViewport(device, &vp);}
        IDirect3DDevice9_Clear(device, 0, NULL, D3DCLEAR_TARGET, 0xff000000, 1, 0);
        if (up) {
            const struct vertex v[3] = {{4,4,0,1,0xffff0000u},{60,4,0,1,0xffff0000u},{32,60,0,1,0xffff0000u}};
            IDirect3DDevice9_SetFVF(device, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
            IDirect3DDevice9_BeginScene(device);
            IDirect3DDevice9_DrawPrimitiveUP(device, D3DPT_TRIANGLELIST, 1, v, sizeof(v[0]));
            IDirect3DDevice9_EndScene(device);
        } else {
            /* Checked buffer path: every HRESULT and the bound state are reported. */
            HRESULT h[8]; IDirect3DVertexBuffer9 *bound = NULL; IDirect3DIndexBuffer9 *bound_ib = NULL; UINT off = 9, stride = 9; DWORD fvf = 0;
            h[0] = IDirect3DDevice9_SetFVF(device, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
            h[1] = IDirect3DDevice9_SetStreamSource(device, 0, vb, 0, sizeof(struct vertex));
            h[2] = IDirect3DDevice9_SetIndices(device, ib);
            h[3] = IDirect3DDevice9_BeginScene(device);
            h[4] = IDirect3DDevice9_DrawIndexedPrimitive(device, D3DPT_TRIANGLELIST, 0, 0, 3, 0, 1);
            h[5] = IDirect3DDevice9_DrawPrimitive(device, D3DPT_TRIANGLELIST, 0, 1);
            h[6] = IDirect3DDevice9_EndScene(device);
            h[7] = IDirect3DDevice9_GetStreamSource(device, 0, &bound, &off, &stride);
            IDirect3DDevice9_GetIndices(device, &bound_ib); IDirect3DDevice9_GetFVF(device, &fvf);
            printf("PW_TRANSFORM_BUFFER_CHECK fvf=%08lx stream=%08lx indices=%08lx begin=%08lx dip=%08lx dp=%08lx end=%08lx get=%08lx same_vb=%d same_ib=%d offset=%u stride=%u bound_fvf=%08lx\n",
                h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7], bound == vb, bound_ib == ib, off, stride, fvf);
            if (bound) IDirect3DVertexBuffer9_Release(bound);
            if (bound_ib) IDirect3DIndexBuffer9_Release(bound_ib);
        }
        hr = IDirect3DDevice9_GetRenderTargetData(device, target, copy);
    }
    if (SUCCEEDED(hr) && SUCCEEDED(hr = IDirect3DSurface9_LockRect(copy, &lock, NULL, D3DLOCK_READONLY))) {
        memcpy(&center, (char *)lock.pBits + 20 * lock.Pitch + 20 * 4, 4);
        for (unsigned y = 0; y < 64; y++) h ^= fnv((char *)lock.pBits + y * lock.Pitch, 64 * 4) + y;
        IDirect3DSurface9_UnlockRect(copy);
    }
    if (original) { IDirect3DDevice9_SetRenderTarget(device, 0, original); IDirect3DSurface9_Release(original); }
    if (copy) IDirect3DSurface9_Release(copy);
    if (target) IDirect3DSurface9_Release(target);
    printf("PW_TRANSFORM_READBACK label=%s source=%s hr=%08lx center=%08lx image=%016llx\n", label, up ? "up" : "buffers", hr, center, (unsigned long long)h);
}
static void step(void)
{
    D3DMATRIX m; HRESULT hr = S_OK; uint32_t state = pick_state(); unsigned op = next() % 100, b = next() % BLOCKS;
    step_no++;
    if (op < 35) { pick_matrix(&m); hr = IDirect3DDevice9_SetTransform(device, state, next() % 16 ? &m : NULL); }
    else if (op < 40) { pick_matrix(&m); hr = IDirect3DDevice9_MultiplyTransform(device, state, &m); }
    else if (op < 62) { get(state); return; }
    else if (op < 66) hr = IDirect3DDevice9_BeginStateBlock(device);
    else if (op < 71) {
        IDirect3DStateBlock9 *sb = NULL;
        hr = IDirect3DDevice9_EndStateBlock(device, &sb);
        if (sb) { if (blocks[b]) IDirect3DStateBlock9_Release(blocks[b]); blocks[b] = sb; }
    } else if (op < 76) {
        static const D3DSTATEBLOCKTYPE types[] = {D3DSBT_ALL, D3DSBT_PIXELSTATE, D3DSBT_VERTEXSTATE};
        IDirect3DStateBlock9 *sb = NULL;
        hr = IDirect3DDevice9_CreateStateBlock(device, types[next() % 3], &sb);
        if (sb) { if (blocks[b]) IDirect3DStateBlock9_Release(blocks[b]); blocks[b] = sb; }
    } else if (op < 82) { if (blocks[b]) hr = IDirect3DStateBlock9_Capture(blocks[b]); }
    else if (op < 92) { if (blocks[b]) hr = IDirect3DStateBlock9_Apply(blocks[b]); }
    else if (op < 95) { if (blocks[b]) { IDirect3DStateBlock9_Release(blocks[b]); blocks[b] = NULL; } }
    else if (op < 99) { if (next() % 8) draw(); else readback("step"); }
    else if (next() % 4 == 0) {
        if (next() % 2) for (unsigned i = 0; i < BLOCKS; i++) if (blocks[i]) { IDirect3DStateBlock9_Release(blocks[i]); blocks[i] = NULL; }
        D3DPRESENT_PARAMETERS copy = pp;
        hr = IDirect3DDevice9_Reset(device, &copy);
    } else hr = IDirect3DDevice9_Present(device, NULL, NULL, NULL, NULL);
    printf("PW_TRANSFORM_OP step=%u op=%u hr=%08lx\n", step_no, op, hr);
}
static LRESULT CALLBACK proc(HWND w, UINT m, WPARAM a, LPARAM b) { return DefWindowProcW(w, m, a, b); }
int wmain(int argc, WCHAR **argv)
{
    if (argc != 6) return 2;
    unsigned steps = (unsigned)wcstoul(argv[4], NULL, 0);
    rng = (uint32_t)wcstoul(argv[5], NULL, 0) | 1u;
    if (!SetEnvironmentVariableW(L"PW_D3D9_SERVICE64", argv[2]) || !SetEnvironmentVariableW(L"PW_D3D9_BACKEND64", argv[3])) return 3;
    HMODULE dll = LoadLibraryW(argv[1]);
    IDirect3D9 *(WINAPI *create)(UINT) = dll ? (void *)GetProcAddress(dll, "Direct3DCreate9") : NULL;
    IDirect3D9 *factory = create ? create(D3D_SDK_VERSION) : NULL;
    if (!factory) return 4;
    WNDCLASSW cls = {.lpfnWndProc = proc, .hInstance = GetModuleHandleW(NULL), .lpszClassName = L"PW_TRANSFORM_CLIENT"};
    if (!RegisterClassW(&cls)) return 5;
    HWND window = CreateWindowW(cls.lpszClassName, L"transform client", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, NULL, NULL, cls.hInstance, NULL);
    pp = (D3DPRESENT_PARAMETERS){.BackBufferWidth = 64, .BackBufferHeight = 64, .BackBufferFormat = D3DFMT_A8R8G8B8, .BackBufferCount = 1,
        .SwapEffect = D3DSWAPEFFECT_DISCARD, .hDeviceWindow = window, .Windowed = TRUE};
    HRESULT hr = IDirect3D9_CreateDevice(factory, 0, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &device);
    if (FAILED(hr)) { printf("PW_TRANSFORM_CLIENT create=%08lx\n", hr); return 6; }
    {
        const struct vertex v[3] = {{4,4,0,1,0xffff0000u},{60,4,0,1,0xffff0000u},{32,60,0,1,0xffff0000u}};
        const WORD index[3] = {0, 1, 2}; void *memory;
        if (FAILED(IDirect3DDevice9_CreateVertexBuffer(device, sizeof(v), 0, D3DFVF_XYZRHW | D3DFVF_DIFFUSE, D3DPOOL_MANAGED, &vb, NULL)) ||
            FAILED(IDirect3DVertexBuffer9_Lock(vb, 0, sizeof(v), &memory, 0))) return 7;
        memcpy(memory, v, sizeof(v)); IDirect3DVertexBuffer9_Unlock(vb);
        if (FAILED(IDirect3DDevice9_CreateIndexBuffer(device, sizeof(index), 0, D3DFMT_INDEX16, D3DPOOL_MANAGED, &ib, NULL)) ||
            FAILED(IDirect3DIndexBuffer9_Lock(ib, 0, sizeof(index), &memory, 0))) return 8;
        memcpy(memory, index, sizeof(index)); IDirect3DIndexBuffer9_Unlock(ib);
    }
    readback("start");
    /* Deterministic edges: fresh-device read, NULL identity, recorded/ALL restore,
     * refused Capture/Apply while recording, failed and successful Reset. */
    IDirect3DStateBlock9 *all = NULL, *recorded = NULL; D3DMATRIX m;
    get(2);
    IDirect3DDevice9_SetTransform(device, 2, NULL); get(2);
    pick_matrix(&m); IDirect3DDevice9_SetTransform(device, 511, &m); get(511);
    IDirect3DDevice9_CreateStateBlock(device, D3DSBT_ALL, &all);
    IDirect3DDevice9_BeginStateBlock(device);
    pick_matrix(&m); IDirect3DDevice9_SetTransform(device, 3, &m); get(3);
    printf("PW_TRANSFORM_EDGE apply_recording=%08lx capture_recording=%08lx\n", IDirect3DStateBlock9_Apply(all), IDirect3DStateBlock9_Capture(all));
    IDirect3DDevice9_EndStateBlock(device, &recorded); get(3);
    pick_matrix(&m); IDirect3DDevice9_SetTransform(device, 3, &m);
    IDirect3DStateBlock9_Apply(recorded); get(3);
    pick_matrix(&m); IDirect3DDevice9_SetTransform(device, 2, &m);
    IDirect3DStateBlock9_Apply(all); get(2); get(511);
    D3DPRESENT_PARAMETERS copy = pp;
    printf("PW_TRANSFORM_EDGE reset_live_blocks=%08lx\n", IDirect3DDevice9_Reset(device, &copy)); get(2);
    IDirect3DStateBlock9_Release(all); IDirect3DStateBlock9_Release(recorded);
    copy = pp;
    printf("PW_TRANSFORM_EDGE reset=%08lx\n", IDirect3DDevice9_Reset(device, &copy)); get(2); get(511);
    for (unsigned i = 0; i < steps; i++) step();
    for (unsigned i = 0; i < BLOCKS; i++) if (blocks[i]) { IDirect3DStateBlock9_Release(blocks[i]); blocks[i] = NULL; }
    {IDirect3DStateBlock9 *sb = NULL; IDirect3DDevice9_EndStateBlock(device, &sb); if (sb) IDirect3DStateBlock9_Release(sb);}
    D3DPRESENT_PARAMETERS last = pp;
    printf("PW_TRANSFORM_EDGE final_reset=%08lx\n", IDirect3DDevice9_Reset(device, &last));
    /* Controlled valid-state stage, isolated between two Present boundaries so
     * its transport interval shows whether the buffer draws were batched. */
    IDirect3DDevice9_Present(device, NULL, NULL, NULL, NULL);
    readback("end");
    IDirect3DDevice9_Present(device, NULL, NULL, NULL, NULL);
    IDirect3DDevice9_SetStreamSource(device, 0, NULL, 0, 0); IDirect3DDevice9_SetIndices(device, NULL);
    IDirect3DVertexBuffer9_Release(vb); IDirect3DIndexBuffer9_Release(ib);
    for (uint32_t s = 2; s < 512; s++) if ((s <= 3) || (s >= 16 && s <= 23) || s >= 256) get(s);
    IDirect3DDevice9_Present(device, NULL, NULL, NULL, NULL);
    ULONG device_refs = IDirect3DDevice9_Release(device), factory_refs = IDirect3D9_Release(factory);
    printf("PW_TRANSFORM_CLIENT steps=%u gets=%u device_refs=%lu factory_refs=%lu status=0\n", steps, get_count, device_refs, factory_refs);
    fflush(stdout);
    DestroyWindow(window);
    return device_refs || factory_refs;
}
