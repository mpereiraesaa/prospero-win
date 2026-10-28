/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Direct3D 12 control: vkd3d-proton's d3d12 over DXVK's dxgi, as a game
 * reaches them (tests/test_d3d12_control.py runs it under the host Wine;
 * on the console it is the same program, run as a game).
 *
 * It creates a device at feature level 11_0, reports what the device
 * offers (the highest feature level, shader model and the tiers games
 * check), makes a 1920x1080 flip-model swapchain on a window, and for two
 * frames clears the back buffer, draws a triangle with shaders compiled on
 * the spot (d3dcompiler_47, vs_5_0/ps_5_0), copies the back buffer to a
 * readback buffer, checks its centre (the triangle) and a corner (the
 * clear colour), and presents. One line on stdout says it all; the exit
 * code is 0 when every step passed.
 */
#define COBJMACROS
#define WIDL_C_INLINE_WRAPPERS
#define INITGUID
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <string.h>

enum { WIDTH = 1920, HEIGHT = 1080, FRAMES = 2 };

/* Newer than MinGW's d3d12.h: the documented values and layouts. */
enum { LEVEL_12_2 = 0xc200, FEATURE_OPTIONS6 = 30, FEATURE_OPTIONS7 = 32 };
typedef struct Options6 {
    BOOL additional_shading_rates, per_primitive_shading_rate_with_viewport_indexing;
    UINT variable_shading_rate_tier, shading_rate_image_tile_size;
    BOOL background_processing;
} Options6;
typedef struct Options7 { UINT mesh_shader_tier, sampler_feedback_tier; } Options7;

/* B8G8R8A8 read as a little-endian word: 0xAARRGGBB. */
static const float clear_colors[FRAMES][4] = {
    { 28.0f / 255.0f, 76.0f / 255.0f, 132.0f / 255.0f, 1.0f },
    { 132.0f / 255.0f, 76.0f / 255.0f, 28.0f / 255.0f, 1.0f },
};
static const UINT32 clear_words[FRAMES] = { 0xff1c4c84u, 0xff844c1cu };
static const UINT32 triangle_word = 0xffff0000u;

static const char shader_source[] =
    "float4 vs(uint id : SV_VertexID) : SV_Position\n"
    "{\n"
    "    float2 p[3] = { float2(-0.5, -0.5), float2(0.0, 0.5), float2(0.5, -0.5) };\n"
    "    return float4(p[id], 0.0, 1.0);\n"
    "}\n"
    "float4 ps() : SV_Target { return float4(1.0, 0.0, 0.0, 1.0); }\n";

static HWND make_window(void)
{
    WNDCLASSW cls = { 0 };
    HWND window;

    cls.lpfnWndProc = DefWindowProcW;
    cls.hInstance = GetModuleHandleW(NULL);
    cls.lpszClassName = L"PW_D3D12_CONTROL";
    if (!RegisterClassW(&cls)) return NULL;
    window = CreateWindowExW(0, cls.lpszClassName, L"Direct3D 12 / prospero-win", WS_POPUP | WS_VISIBLE,
                             0, 0, WIDTH, HEIGHT, NULL, NULL, cls.hInstance, NULL);
    if (window) {
        ShowWindow(window, SW_SHOW);
        UpdateWindow(window);
    }
    return window;
}

static void pump(void)
{
    MSG message;

    while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

static D3D12_RESOURCE_BARRIER transition(ID3D12Resource *resource, D3D12_RESOURCE_STATES before,
                                         D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier;

    memset(&barrier, 0, sizeof(barrier));
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    return barrier;
}

/* The device's answers to what games ask before they pick a path. */
typedef struct Caps {
    UINT max_level, shader_model, binding, tiled, rov, conservative, raytracing, mesh, vrs;
} Caps;

static void query_caps(ID3D12Device *device, Caps *caps)
{
    static const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_11_1,
                                                D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_12_1,
                                                (D3D_FEATURE_LEVEL)LEVEL_12_2 };
    D3D12_FEATURE_DATA_FEATURE_LEVELS level_data = { sizeof(levels) / sizeof(levels[0]), levels, 0 };
    D3D12_FEATURE_DATA_SHADER_MODEL model;
    D3D12_FEATURE_DATA_D3D12_OPTIONS options;
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5;
    Options6 options6;
    Options7 options7;

    memset(caps, 0, sizeof(*caps));
    if (SUCCEEDED(ID3D12Device_CheckFeatureSupport(device, D3D12_FEATURE_FEATURE_LEVELS, &level_data,
                                                   sizeof(level_data))))
        caps->max_level = level_data.MaxSupportedFeatureLevel;
    /* The highest model this header may not name yet: asked from 6.8 down. */
    for (UINT asked = 0x68; asked >= 0x50 && !caps->shader_model; asked = asked == 0x60 ? 0x51 : asked - 1) {
        model.HighestShaderModel = (D3D_SHADER_MODEL)asked;
        if (SUCCEEDED(ID3D12Device_CheckFeatureSupport(device, D3D12_FEATURE_SHADER_MODEL, &model, sizeof(model))))
            caps->shader_model = model.HighestShaderModel;
        if (asked == 0x51) break;
    }
    if (SUCCEEDED(ID3D12Device_CheckFeatureSupport(device, D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)))) {
        caps->binding = options.ResourceBindingTier;
        caps->tiled = options.TiledResourcesTier;
        caps->rov = options.ROVsSupported;
        caps->conservative = options.ConservativeRasterizationTier;
    }
    if (SUCCEEDED(ID3D12Device_CheckFeatureSupport(device, D3D12_FEATURE_D3D12_OPTIONS5, &options5,
                                                   sizeof(options5))))
        caps->raytracing = options5.RaytracingTier;
    if (SUCCEEDED(ID3D12Device_CheckFeatureSupport(device, (D3D12_FEATURE)FEATURE_OPTIONS6, &options6,
                                                   sizeof(options6))))
        caps->vrs = options6.variable_shading_rate_tier;
    if (SUCCEEDED(ID3D12Device_CheckFeatureSupport(device, (D3D12_FEATURE)FEATURE_OPTIONS7, &options7,
                                                   sizeof(options7))))
        caps->mesh = options7.mesh_shader_tier;
}

static HRESULT make_pipeline(ID3D12Device *device, ID3D12RootSignature **root, ID3D12PipelineState **pipeline)
{
    D3D12_ROOT_SIGNATURE_DESC root_desc;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc;
    ID3DBlob *signature = NULL, *vs = NULL, *ps = NULL, *errors = NULL;
    HRESULT hr;

    memset(&root_desc, 0, sizeof(root_desc));
    hr = D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &errors);
    if (SUCCEEDED(hr))
        hr = ID3D12Device_CreateRootSignature(device, 0, ID3D10Blob_GetBufferPointer(signature),
                                              ID3D10Blob_GetBufferSize(signature), &IID_ID3D12RootSignature,
                                              (void **)root);
    if (SUCCEEDED(hr))
        hr = D3DCompile(shader_source, sizeof(shader_source) - 1, "control", NULL, NULL, "vs", "vs_5_0", 0, 0,
                        &vs, &errors);
    if (SUCCEEDED(hr))
        hr = D3DCompile(shader_source, sizeof(shader_source) - 1, "control", NULL, NULL, "ps", "ps_5_0", 0, 0,
                        &ps, &errors);
    if (SUCCEEDED(hr)) {
        memset(&desc, 0, sizeof(desc));
        desc.pRootSignature = *root;
        desc.VS.pShaderBytecode = ID3D10Blob_GetBufferPointer(vs);
        desc.VS.BytecodeLength = ID3D10Blob_GetBufferSize(vs);
        desc.PS.pShaderBytecode = ID3D10Blob_GetBufferPointer(ps);
        desc.PS.BytecodeLength = ID3D10Blob_GetBufferSize(ps);
        desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        desc.SampleMask = UINT_MAX;
        desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        desc.RasterizerState.DepthClipEnable = TRUE;
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.NumRenderTargets = 1;
        desc.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        hr = ID3D12Device_CreateGraphicsPipelineState(device, &desc, &IID_ID3D12PipelineState, (void **)pipeline);
    }
    if (errors) {
        fprintf(stderr, "d3d12 control: %.*s\n", (int)ID3D10Blob_GetBufferSize(errors),
                (const char *)ID3D10Blob_GetBufferPointer(errors));
        ID3D10Blob_Release(errors);
    }
    if (signature) ID3D10Blob_Release(signature);
    if (vs) ID3D10Blob_Release(vs);
    if (ps) ID3D10Blob_Release(ps);
    return hr;
}

int main(void)
{
    HWND window = make_window();
    IDXGIFactory4 *factory = NULL;
    ID3D12Device *device = NULL;
    ID3D12CommandQueue *queue = NULL;
    ID3D12CommandAllocator *allocator = NULL;
    ID3D12GraphicsCommandList *list = NULL;
    ID3D12Fence *fence = NULL;
    IDXGISwapChain1 *swapchain1 = NULL;
    IDXGISwapChain3 *swapchain = NULL;
    ID3D12DescriptorHeap *rtv_heap = NULL;
    ID3D12Resource *buffers[FRAMES] = { NULL }, *readback = NULL;
    ID3D12RootSignature *root = NULL;
    ID3D12PipelineState *pipeline = NULL;
    HANDLE event = CreateEventW(NULL, FALSE, FALSE, NULL);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
    UINT64 readback_size = 0, fence_value = 0;
    UINT rtv_step = 0;
    UINT32 centre[FRAMES] = { 0 }, corner[FRAMES] = { 0 };
    HRESULT factory_hr, device_hr = E_FAIL, queue_hr = E_FAIL, swap_hr = E_FAIL, pipeline_hr = E_FAIL;
    HRESULT frame_hr[FRAMES] = { E_FAIL, E_FAIL }, present_hr[FRAMES] = { E_FAIL, E_FAIL };
    Caps caps;
    int pass;

    memset(&caps, 0, sizeof(caps));
    if (!window || !event) return 2;
    factory_hr = CreateDXGIFactory1(&IID_IDXGIFactory4, (void **)&factory);
    if (SUCCEEDED(factory_hr))
        device_hr = D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&device);
    if (SUCCEEDED(device_hr)) {
        D3D12_COMMAND_QUEUE_DESC queue_desc;

        query_caps(device, &caps);
        memset(&queue_desc, 0, sizeof(queue_desc));
        queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        queue_hr = ID3D12Device_CreateCommandQueue(device, &queue_desc, &IID_ID3D12CommandQueue, (void **)&queue);
        if (SUCCEEDED(queue_hr))
            queue_hr = ID3D12Device_CreateCommandAllocator(device, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                           &IID_ID3D12CommandAllocator, (void **)&allocator);
        if (SUCCEEDED(queue_hr))
            queue_hr = ID3D12Device_CreateCommandList(device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, NULL,
                                                      &IID_ID3D12GraphicsCommandList, (void **)&list);
        if (SUCCEEDED(queue_hr)) queue_hr = ID3D12GraphicsCommandList_Close(list);
        if (SUCCEEDED(queue_hr))
            queue_hr = ID3D12Device_CreateFence(device, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence);
    }
    if (SUCCEEDED(queue_hr)) {
        DXGI_SWAP_CHAIN_DESC1 desc;
        D3D12_DESCRIPTOR_HEAP_DESC heap_desc;

        memset(&desc, 0, sizeof(desc));
        desc.Width = WIDTH;
        desc.Height = HEIGHT;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = FRAMES;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        swap_hr = IDXGIFactory4_CreateSwapChainForHwnd(factory, (IUnknown *)queue, window, &desc, NULL, NULL,
                                                       &swapchain1);
        if (SUCCEEDED(swap_hr))
            swap_hr = IDXGISwapChain1_QueryInterface(swapchain1, &IID_IDXGISwapChain3, (void **)&swapchain);
        memset(&heap_desc, 0, sizeof(heap_desc));
        heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heap_desc.NumDescriptors = FRAMES;
        if (SUCCEEDED(swap_hr))
            swap_hr = ID3D12Device_CreateDescriptorHeap(device, &heap_desc, &IID_ID3D12DescriptorHeap,
                                                        (void **)&rtv_heap);
        rtv_step = SUCCEEDED(swap_hr) ?
                   ID3D12Device_GetDescriptorHandleIncrementSize(device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV) : 0;
        for (UINT i = 0; i < FRAMES && SUCCEEDED(swap_hr); i++) {
            D3D12_CPU_DESCRIPTOR_HANDLE rtv = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(rtv_heap);

            rtv.ptr += (SIZE_T)i * rtv_step;
            swap_hr = IDXGISwapChain3_GetBuffer(swapchain, i, &IID_ID3D12Resource, (void **)&buffers[i]);
            if (SUCCEEDED(swap_hr)) ID3D12Device_CreateRenderTargetView(device, buffers[i], NULL, rtv);
        }
    }
    if (SUCCEEDED(swap_hr)) {
        D3D12_RESOURCE_DESC texture = ID3D12Resource_GetDesc(buffers[0]), buffer;
        D3D12_HEAP_PROPERTIES heap;

        ID3D12Device_GetCopyableFootprints(device, &texture, 0, 1, 0, &footprint, NULL, NULL, &readback_size);
        memset(&heap, 0, sizeof(heap));
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        memset(&buffer, 0, sizeof(buffer));
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = readback_size;
        buffer.Height = 1;
        buffer.DepthOrArraySize = 1;
        buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        pipeline_hr = ID3D12Device_CreateCommittedResource(device, &heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                           D3D12_RESOURCE_STATE_COPY_DEST, NULL,
                                                           &IID_ID3D12Resource, (void **)&readback);
        if (SUCCEEDED(pipeline_hr)) pipeline_hr = make_pipeline(device, &root, &pipeline);
    }
    for (int frame = 0; frame < FRAMES && SUCCEEDED(pipeline_hr); frame++) {
        UINT index = IDXGISwapChain3_GetCurrentBackBufferIndex(swapchain);
        ID3D12Resource *target = buffers[index];
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(rtv_heap);
        D3D12_VIEWPORT viewport = { 0.0f, 0.0f, (float)WIDTH, (float)HEIGHT, 0.0f, 1.0f };
        D3D12_RECT scissor = { 0, 0, WIDTH, HEIGHT };
        D3D12_TEXTURE_COPY_LOCATION source, destination;
        D3D12_RESOURCE_BARRIER barrier;
        ID3D12CommandList *lists[1];
        const BYTE *mapped = NULL;
        D3D12_RANGE range = { 0, (SIZE_T)readback_size };

        rtv.ptr += (SIZE_T)index * rtv_step;
        frame_hr[frame] = ID3D12CommandAllocator_Reset(allocator);
        if (SUCCEEDED(frame_hr[frame])) frame_hr[frame] = ID3D12GraphicsCommandList_Reset(list, allocator, pipeline);
        if (FAILED(frame_hr[frame])) break;
        barrier = transition(target, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &barrier);
        ID3D12GraphicsCommandList_ClearRenderTargetView(list, rtv, clear_colors[frame], 0, NULL);
        ID3D12GraphicsCommandList_SetGraphicsRootSignature(list, root);
        ID3D12GraphicsCommandList_RSSetViewports(list, 1, &viewport);
        ID3D12GraphicsCommandList_RSSetScissorRects(list, 1, &scissor);
        ID3D12GraphicsCommandList_OMSetRenderTargets(list, 1, &rtv, FALSE, NULL);
        ID3D12GraphicsCommandList_IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D12GraphicsCommandList_DrawInstanced(list, 3, 1, 0, 0);
        barrier = transition(target, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &barrier);
        memset(&source, 0, sizeof(source));
        source.pResource = target;
        source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        memset(&destination, 0, sizeof(destination));
        destination.pResource = readback;
        destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        destination.PlacedFootprint = footprint;
        ID3D12GraphicsCommandList_CopyTextureRegion(list, &destination, 0, 0, 0, &source, NULL);
        barrier = transition(target, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
        ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &barrier);
        frame_hr[frame] = ID3D12GraphicsCommandList_Close(list);
        if (FAILED(frame_hr[frame])) break;
        lists[0] = (ID3D12CommandList *)list;
        ID3D12CommandQueue_ExecuteCommandLists(queue, 1, lists);
        frame_hr[frame] = ID3D12CommandQueue_Signal(queue, fence, ++fence_value);
        if (SUCCEEDED(frame_hr[frame]))
            frame_hr[frame] = ID3D12Fence_SetEventOnCompletion(fence, fence_value, event);
        if (SUCCEEDED(frame_hr[frame]) && WaitForSingleObject(event, 5000) != WAIT_OBJECT_0)
            frame_hr[frame] = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        if (SUCCEEDED(frame_hr[frame]))
            frame_hr[frame] = ID3D12Resource_Map(readback, 0, &range, (void **)&mapped);
        if (SUCCEEDED(frame_hr[frame])) {
            const BYTE *row = mapped + footprint.Offset + (SIZE_T)(HEIGHT / 2) * footprint.Footprint.RowPitch;
            memcpy(&centre[frame], row + (SIZE_T)(WIDTH / 2) * 4, 4);
            memcpy(&corner[frame], mapped + footprint.Offset, 4);
            ID3D12Resource_Unmap(readback, 0, NULL);
        }
        present_hr[frame] = IDXGISwapChain3_Present(swapchain, 1, 0);
        pump();
        Sleep(250);
    }

    pass = SUCCEEDED(device_hr) && SUCCEEDED(queue_hr) && SUCCEEDED(swap_hr) && SUCCEEDED(pipeline_hr);
    for (int frame = 0; frame < FRAMES; frame++)
        pass = pass && SUCCEEDED(frame_hr[frame]) && SUCCEEDED(present_hr[frame]) &&
               centre[frame] == triangle_word && corner[frame] == clear_words[frame];
    printf("D3D12_CONTROL factory=0x%08lx device=0x%08lx max_level=0x%04x shader_model=0x%02x binding=%u "
           "tiled=%u rov=%u conservative=%u raytracing=%u mesh=%u vrs=%u queue=0x%08lx swap=0x%08lx "
           "pipeline=0x%08lx",
           (unsigned long)factory_hr, (unsigned long)device_hr, caps.max_level, caps.shader_model, caps.binding,
           caps.tiled, caps.rov, caps.conservative, caps.raytracing, caps.mesh, caps.vrs,
           (unsigned long)queue_hr, (unsigned long)swap_hr, (unsigned long)pipeline_hr);
    for (int frame = 0; frame < FRAMES; frame++)
        printf(" frame%d=0x%08lx centre%d=%08x corner%d=%08x present%d=0x%08lx", frame,
               (unsigned long)frame_hr[frame], frame, (unsigned)centre[frame], frame, (unsigned)corner[frame],
               frame, (unsigned long)present_hr[frame]);
    printf(" verdict=%s\n", pass ? "pass" : "fail");
    fflush(stdout);

    if (pipeline) ID3D12PipelineState_Release(pipeline);
    if (root) ID3D12RootSignature_Release(root);
    if (readback) ID3D12Resource_Release(readback);
    for (int i = 0; i < FRAMES; i++)
        if (buffers[i]) ID3D12Resource_Release(buffers[i]);
    if (rtv_heap) ID3D12DescriptorHeap_Release(rtv_heap);
    if (swapchain) IDXGISwapChain3_Release(swapchain);
    if (swapchain1) IDXGISwapChain1_Release(swapchain1);
    if (fence) ID3D12Fence_Release(fence);
    if (list) ID3D12GraphicsCommandList_Release(list);
    if (allocator) ID3D12CommandAllocator_Release(allocator);
    if (queue) ID3D12CommandQueue_Release(queue);
    if (device) ID3D12Device_Release(device);
    if (factory) IDXGIFactory4_Release(factory);
    DestroyWindow(window);
    CloseHandle(event);
    return pass ? 0 : 1;
}
