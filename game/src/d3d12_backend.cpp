#include <dxgi1_4.h>
#include <d3d12.h>

#include "renderer.h"
#include "platform.h"

struct Renderer {
    IDXGIFactory3* factory;
    IDXGIAdapter* adapter;
    ID3D12Device* device;
};

Renderer* rd_init(Arena* arena) {
    Renderer* r = arena->push_type<Renderer>();

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

    return r;
}

void rd_free(Renderer* r) {
    r->device->Release();
    r->adapter->Release();
    r->factory->Release();
}
