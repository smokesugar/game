#include <dxgi1_4.h>
#include "agility/d3d12.h"

#include "renderer.h"
#include "platform.h"

extern "C" { __declspec(dllexport) extern const UINT D3D12SDKVersion = 608;}
extern "C" { __declspec(dllexport) extern const char* D3D12SDKPath = ".\\d3d12\\"; }

struct Queue {
    ID3D12CommandQueue* queue;
    u64 fence_val;
    ID3D12Fence* fence;

    inline u64 signal() {
        u64 val = ++fence_val;
        queue->Signal(fence, val);
        return val;
    }

    inline void wait(u64 val) {
        if (fence->GetCompletedValue() < val) {
            fence->SetEventOnCompletion(val, 0);
        }
    }

    inline void flush() {
        wait(signal());
    }

    inline void free() {
        queue->Release();
        fence->Release();
    }
};

struct Renderer {
    HWND window;

    IDXGIFactory3* factory;
    IDXGIAdapter* adapter;
    ID3D12Device* device;

    Queue direct_queue;

    IDXGISwapChain3* swapchain;
    u32 swapchain_buffer_count;
    u32 swapchain_w, swapchain_h;
    u64 swapchain_fences[DXGI_MAX_SWAP_CHAIN_BUFFERS];
    ID3D12Resource* swapchain_buffers[DXGI_MAX_SWAP_CHAIN_BUFFERS];

    inline void get_swapchain_buffers() {
        for (u32 i = 0; i < swapchain_buffer_count; ++i) {
            swapchain->GetBuffer(i, IID_PPV_ARGS(&swapchain_buffers[i]));
        }
    }

    inline void free_swapchain_buffers() {
        for (u32 i = 0; i < swapchain_buffer_count; ++i) {
            swapchain_buffers[i]->Release();
       }
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

static void queue_init(ID3D12Device* device, D3D12_COMMAND_LIST_TYPE type, Queue* queue) {
    memset(queue, 0, sizeof(*queue));

    D3D12_COMMAND_QUEUE_DESC queue_desc = {};
    queue_desc.Type = type;

    device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue->queue));
    device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&queue->fence));
}

Renderer* rd_init(Arena* arena, void* window) {
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

    queue_init(r->device, D3D12_COMMAND_LIST_TYPE_DIRECT, &r->direct_queue);

    auto [window_w, window_h] = hwnd_size((HWND)window);
    
    r->swapchain_buffer_count = 2;
    r->swapchain_w = window_w;
    r->swapchain_h = window_h;

    DXGI_SWAP_CHAIN_DESC1 swapchain_desc = {};
    swapchain_desc.Width = window_w;
    swapchain_desc.Height = window_h;
    swapchain_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapchain_desc.SampleDesc.Count = 1;
    swapchain_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapchain_desc.BufferCount = r->swapchain_buffer_count;
    swapchain_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    IDXGISwapChain1* swapchain = 0;
    r->factory->CreateSwapChainForHwnd(r->direct_queue.queue, (HWND)window, &swapchain_desc, 0, 0, &swapchain);
    swapchain->QueryInterface(&r->swapchain);
    swapchain->Release();

    r->get_swapchain_buffers();

    return r;
}

void rd_free(Renderer* r) {
    r->direct_queue.flush();

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
    r->swapchain->Present(1, 0);
    r->swapchain_fences[swapchain_index] = r->direct_queue.signal();
}
    
