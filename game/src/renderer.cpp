#include <dxgi1_4.h>
#include "agility/d3d12.h"

#include "renderer.h"
#include "platform.h"

#define MAX_RTV_COUNT 1024

extern "C" { __declspec(dllexport) extern const UINT D3D12SDKVersion = 608;}
extern "C" { __declspec(dllexport) extern const char* D3D12SDKPath = ".\\d3d12\\"; }

struct CommandList {
    ID3D12GraphicsCommandList* list;
    ID3D12CommandAllocator* allocator;
    u64 fence_val;

    ID3D12GraphicsCommandList* operator->() {
        return list;
    }
};

struct Queue {
    ID3D12CommandQueue* queue;
    u64 fence_val;
    ID3D12Fence* fence;
    Vec<CommandList> occupied_command_lists;

    void init(ID3D12Device* device, D3D12_COMMAND_LIST_TYPE type) {
        D3D12_COMMAND_QUEUE_DESC queue_desc = {};
        queue_desc.Type = type;

        device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue));

        fence_val = 0;
        device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    }

    void free() {
        queue->Release();
        fence->Release();
        occupied_command_lists.free();
    }

    u64 signal() {
        u64 val = ++fence_val;
        queue->Signal(fence, val);
        return val;
    }

    void wait(u64 val) {
        if (fence->GetCompletedValue() < val) {
            fence->SetEventOnCompletion(val, 0);
        }
    }

    bool reached(u64 val) {
        return fence->GetCompletedValue() >= val;
    }

    void flush() {
        wait(signal());
    }

    void submit_command_list(CommandList list) {
        list.list->Close();
        ID3D12CommandList* p_list = list.list;
        queue->ExecuteCommandLists(1, &p_list);
        list.fence_val = signal();
        occupied_command_lists.push(list);
    }

    void poll_command_lists(Vec<CommandList>* avail) {
        for (int i = occupied_command_lists.len-1; i >= 0; --i)
        {
            CommandList list = occupied_command_lists[i];

            if (reached(list.fence_val)) {
                avail->push(list);
                occupied_command_lists.remove_by_patch(i);
            }
        }
    }
};

struct Descriptor {
    u32 index;
    #if _DEBUG
    u32 generation;
    #endif
};

struct DescriptorHeap {
    u32 capacity;
    ID3D12DescriptorHeap* heap;

    u32 num_free;
    u32* free_list;

    u64 stride;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_base;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_base;

    #if _DEBUG
    u32* generations;
    #endif

    void init(Arena* arena, ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type, u32 count, bool shader_visible) {
        capacity = count;

        D3D12_DESCRIPTOR_HEAP_DESC desc = {};
        desc.NumDescriptors = count;
        desc.Type = type;

        if (shader_visible) {
            desc.Flags |= D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        }

        device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap));

        free_list = arena->push_array<u32>(count);

        #if _DEBUG
        generations = arena->push_array<u32>(count);
        #endif

        for (u32 i = 0; i < count; ++i) {
            free_list[num_free++] = i;
            #if _DEBUG
            generations[i] = 1;
            #endif
        }

        stride = device->GetDescriptorHandleIncrementSize(type);
        cpu_base = heap->GetCPUDescriptorHandleForHeapStart();

        if (shader_visible) {
            gpu_base = heap->GetGPUDescriptorHandleForHeapStart();
        }
    }

    void free() {
        heap->Release();
    }

    void validate_descriptor(Descriptor desc) {
        (void)desc;
        assert(desc.index < capacity);
        assert(desc.generation == generations[desc.index]);
    }

    D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle(Descriptor desc) {
        validate_descriptor(desc);
        return { cpu_base.ptr + desc.index * stride };
    }

    D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle(Descriptor desc) {
        validate_descriptor(desc);
        return { gpu_base.ptr + desc.index * stride };
    }

    Descriptor alloc_descriptor() {
        assert(num_free > 0);
        Descriptor desc = {};
        desc.index = free_list[--num_free];
        #if _DEBUG
        desc.generation = generations[desc.index];
        #endif
        return desc;
    }

    Descriptor create_rtv(ID3D12Device* device, ID3D12Resource* resource, D3D12_RENDER_TARGET_VIEW_DESC* rtv_desc) {
        Descriptor d = alloc_descriptor();
        device->CreateRenderTargetView(resource, rtv_desc, cpu_handle(d));
        return d;
    }

    void free_descriptor(Descriptor desc) {
        validate_descriptor(desc);
        #if _DEBUG
        generations[desc.index]++;
        #endif
        free_list[num_free++] = desc.index;
    }
};

struct Renderer {
    HWND window;

    IDXGIFactory3* factory;
    IDXGIAdapter* adapter;
    ID3D12Device* device;

    Queue direct_queue;

    DescriptorHeap rtv_heap;

    IDXGISwapChain3* swapchain;
    DXGI_FORMAT swapchain_format;
    u32 swapchain_buffer_count;
    u32 swapchain_w, swapchain_h;
    u64 swapchain_fences[DXGI_MAX_SWAP_CHAIN_BUFFERS];
    ID3D12Resource* swapchain_buffers[DXGI_MAX_SWAP_CHAIN_BUFFERS];
    Descriptor swapchain_rtvs[DXGI_MAX_SWAP_CHAIN_BUFFERS];

    Vec<CommandList> available_command_lists;

    ID3D12RootSignature* root_signature;
    ID3D12PipelineState* pipeline;

    void get_swapchain_buffers() {
        for (u32 i = 0; i < swapchain_buffer_count; ++i) {
            swapchain->GetBuffer(i, IID_PPV_ARGS(&swapchain_buffers[i]));

            D3D12_RENDER_TARGET_VIEW_DESC rtv_desc = {};
            rtv_desc.Format = swapchain_format;
            rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;

            swapchain_rtvs[i] = rtv_heap.create_rtv(device, swapchain_buffers[i], &rtv_desc);
        }
    }

    void free_swapchain_buffers() {
        for (u32 i = 0; i < swapchain_buffer_count; ++i) {
            rtv_heap.free_descriptor(swapchain_rtvs[i]);
            swapchain_buffers[i]->Release();
        }
    }

    CommandList open_command_list() {
        direct_queue.poll_command_lists(&available_command_lists);

        if (available_command_lists.empty()) {
            CommandList list = {};
            device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&list.allocator));
            device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, list.allocator, 0, IID_PPV_ARGS(&list.list));
            list.list->Close();
            available_command_lists.push(list);
        }

        CommandList list = available_command_lists.pop();
        list.allocator->Reset();
        list.list->Reset(list.allocator, 0);

        list->SetGraphicsRootSignature(root_signature);

        return list;
    }
};

static Pair<u32, u32> hwnd_size(HWND window) {
    RECT rect;
    GetClientRect(window, &rect);
    return {
        (u32)(rect.right-rect.left),
        (u32)(rect.bottom-rect.top)
    };
}

Renderer* rd_init(Arena* arena, void* window) {
    Scratch scratch = get_scratch(arena);
    Renderer* r = arena->push_type<Renderer>();

    r->window = (HWND)window;

    #if _DEBUG
    {
        ID3D12Debug* debug = 0;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
            debug->EnableDebugLayer();
            debug->Release();
        }
    }
    #endif

    if (FAILED(CreateDXGIFactory(IID_PPV_ARGS(&r->factory)))) {
        pf_msg_box("Failed to create DXGI device");
        return 0;
    }

    if (FAILED(r->factory->EnumAdapters(0, &r->adapter))) {
        pf_msg_box("Failed to find DXGI adapter");
        return 0;
    }

    if (FAILED(D3D12CreateDevice(r->adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&r->device)))) {
        pf_msg_box("Failed to create D3D12 device");
        return 0;
    }

    #if _DEBUG
    {
        ID3D12InfoQueue* info_queue = 0;
        if (SUCCEEDED(r->device->QueryInterface(&info_queue)))
        {
            info_queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, TRUE);
            info_queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, TRUE);
            info_queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, TRUE);

            D3D12_MESSAGE_SEVERITY severity_filter = D3D12_MESSAGE_SEVERITY_INFO;

            D3D12_MESSAGE_ID message_filter[] = {
                D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE,
                D3D12_MESSAGE_ID_CLEARDEPTHSTENCILVIEW_MISMATCHINGCLEARVALUE,
                D3D12_MESSAGE_ID_MAP_INVALID_NULLRANGE,
                D3D12_MESSAGE_ID_UNMAP_INVALID_NULLRANGE,
                D3D12_MESSAGE_ID_CREATEGRAPHICSPIPELINESTATE_DEPTHSTENCILVIEW_NOT_SET
            };

            D3D12_INFO_QUEUE_FILTER filter = {};
            filter.DenyList.NumSeverities = 1;
            filter.DenyList.pSeverityList = &severity_filter;
            filter.DenyList.NumIDs = ARRAY_LEN(message_filter);
            filter.DenyList.pIDList = message_filter;

            info_queue->PushStorageFilter(&filter);
            info_queue->Release();
        }
    }
    #endif

    r->direct_queue.init(r->device, D3D12_COMMAND_LIST_TYPE_DIRECT);

    r->rtv_heap.init(arena, r->device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, MAX_RTV_COUNT, false);

    auto [window_w, window_h] = hwnd_size((HWND)window);
    
    r->swapchain_format = DXGI_FORMAT_R8G8B8A8_UNORM;
    r->swapchain_buffer_count = 2;
    r->swapchain_w = window_w;
    r->swapchain_h = window_h;

    DXGI_SWAP_CHAIN_DESC1 swapchain_desc = {};
    swapchain_desc.Width = window_w;
    swapchain_desc.Height = window_h;
    swapchain_desc.Format = r->swapchain_format;
    swapchain_desc.SampleDesc.Count = 1;
    swapchain_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapchain_desc.BufferCount = r->swapchain_buffer_count;
    swapchain_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    IDXGISwapChain1* swapchain = 0;
    r->factory->CreateSwapChainForHwnd(r->direct_queue.queue, (HWND)window, &swapchain_desc, 0, 0, &swapchain);
    swapchain->QueryInterface(&r->swapchain);
    swapchain->Release();

    r->get_swapchain_buffers();

    D3D12_ROOT_SIGNATURE_DESC root_signature_desc = {};
    ID3DBlob* root_signature_code;
    D3D12SerializeRootSignature(&root_signature_desc, D3D_ROOT_SIGNATURE_VERSION_1, &root_signature_code, 0);
    r->device->CreateRootSignature(0, root_signature_code->GetBufferPointer(), root_signature_code->GetBufferSize(), IID_PPV_ARGS(&r->root_signature));
    root_signature_code->Release();

    FileContents triangle_vs = pf_load_file(scratch.arena, "shaders/triangle_vs.bin");
    FileContents triangle_ps = pf_load_file(scratch.arena, "shaders/triangle_ps.bin");

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline_desc = {};

    pipeline_desc.pRootSignature = r->root_signature;

    pipeline_desc.VS.BytecodeLength  = triangle_vs.size;
    pipeline_desc.VS.pShaderBytecode = triangle_vs.memory;
    pipeline_desc.PS.BytecodeLength  = triangle_ps.size;
    pipeline_desc.PS.pShaderBytecode = triangle_ps.memory;

    for (int i = 0; i < ARRAY_LEN(pipeline_desc.BlendState.RenderTarget); ++i) {
        D3D12_RENDER_TARGET_BLEND_DESC* blend = pipeline_desc.BlendState.RenderTarget + i;
        blend->SrcBlend = D3D12_BLEND_ONE;
        blend->DestBlend = D3D12_BLEND_ZERO;
        blend->BlendOp = D3D12_BLEND_OP_ADD;
        blend->SrcBlendAlpha = D3D12_BLEND_ONE;
        blend->DestBlendAlpha = D3D12_BLEND_ZERO;
        blend->BlendOpAlpha = D3D12_BLEND_OP_ADD;
        blend->LogicOp = D3D12_LOGIC_OP_NOOP;
        blend->RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }

    pipeline_desc.SampleMask = D3D12_DEFAULT_SAMPLE_MASK;

    pipeline_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pipeline_desc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    pipeline_desc.RasterizerState.DepthClipEnable = TRUE;
    pipeline_desc.RasterizerState.FrontCounterClockwise = TRUE;

    pipeline_desc.DepthStencilState.DepthEnable = true;
    pipeline_desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pipeline_desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;

    pipeline_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;

    pipeline_desc.NumRenderTargets = 1;
    pipeline_desc.RTVFormats[0] = r->swapchain_format;
    pipeline_desc.DSVFormat = DXGI_FORMAT_UNKNOWN;;

    pipeline_desc.SampleDesc.Count = 1;

    r->device->CreateGraphicsPipelineState(&pipeline_desc, IID_PPV_ARGS(&r->pipeline));

    return r;
}

void rd_free(Renderer* r) {
    r->direct_queue.flush();

    r->direct_queue.poll_command_lists(&r->available_command_lists);
    assert(r->direct_queue.occupied_command_lists.empty());

    for (u32 i = 0; i < r->available_command_lists.len; ++i) {
        r->available_command_lists[i].list->Release();
        r->available_command_lists[i].allocator->Release();
    }

    r->pipeline->Release();
    r->root_signature->Release();

    r->rtv_heap.free();

    r->free_swapchain_buffers();
    r->swapchain->Release();

    r->direct_queue.free();

    r->device->Release();
    r->adapter->Release();
    r->factory->Release();
}

void rd_render(Renderer* r) {
    auto [window_w, window_h] = hwnd_size(r->window);

    if (window_w == 0 || window_h == 0) {
        return;
    }

    if (r->swapchain_w != window_w || r->swapchain_h != window_h) {
        r->direct_queue.flush();

        r->free_swapchain_buffers();
        r->swapchain->ResizeBuffers(0, window_w, window_h, DXGI_FORMAT_UNKNOWN, 0);
        r->get_swapchain_buffers();

        r->swapchain_w = window_w;
        r->swapchain_h = window_h;
    }

    u32 swapchain_index = r->swapchain->GetCurrentBackBufferIndex();
    r->direct_queue.wait(r->swapchain_fences[swapchain_index]);

    CommandList cmd = r->open_command_list();

    D3D12_RESOURCE_BARRIER resource_barrier = {};
    resource_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    resource_barrier.Transition.pResource = r->swapchain_buffers[swapchain_index];
    resource_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    resource_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    resource_barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmd->ResourceBarrier(1, &resource_barrier);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv_cpu_handle = r->rtv_heap.cpu_handle(r->swapchain_rtvs[swapchain_index]);

    cmd->SetPipelineState(r->pipeline);

    f32 clear_color[4] = { 0.2f, 0.3f, 0.3f, 1.0f };
    cmd->ClearRenderTargetView(rtv_cpu_handle, clear_color, 0, 0);
    cmd->OMSetRenderTargets(1, &rtv_cpu_handle, false, 0);

    D3D12_VIEWPORT viewport = {};
    viewport.Width = (f32)window_w;
    viewport.Height = (f32)window_h;
    viewport.MaxDepth = 1.0f;

    cmd->RSSetViewports(1, &viewport);

    D3D12_RECT scissor = {};
    scissor.right = window_w;
    scissor.bottom = window_h;

    cmd->RSSetScissorRects(1, &scissor);

    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->DrawInstanced(3, 1, 0, 0);

    swap(resource_barrier.Transition.StateBefore, resource_barrier.Transition.StateAfter);
    cmd->ResourceBarrier(1, &resource_barrier);

    r->direct_queue.submit_command_list(cmd);

    r->swapchain->Present(1, 0);
    r->swapchain_fences[swapchain_index] = r->direct_queue.signal();
}
