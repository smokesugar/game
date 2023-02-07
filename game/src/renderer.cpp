#include <dxgi1_4.h>
#include <agility/d3d12.h>

#include <dxc/dxcapi.h>
#include <dxc/d3d12shader.h>

#include "renderer.h"
#include "platform.h"
#include "maps.h"
#include "shader.h"

#define RENDERER_ARENA_SIZE (50 * 1024 * 1024)

#define MAX_RTV_COUNT 1024
#define MAX_CBV_SRV_UAV_COUNT 1000000
#define MAX_DSV_COUNT 1024

#define CONSTANT_BUFFER_CAPACITY 256
#define CONSTANT_BUFFER_POOL_COUNT 256

#define MAX_POINT_LIGHT_COUNT 1024
#define MAX_DIRECTIONAL_LIGHT_COUNT 16

#define DEFAULT_UPLOAD_POOL_SIZE (256 * 256)

#define RENDER_GRAPH_NODE_MAX_INPUT_OUTPUTS 16

extern "C" { __declspec(dllexport) extern const UINT D3D12SDKVersion = 608;}
extern "C" { __declspec(dllexport) extern const char* D3D12SDKPath = ".\\d3d12\\"; }

struct Descriptor {
    u32 index;
    u32 generation;
};

struct DescriptorHeap {
    u32 capacity;
    D3D12_DESCRIPTOR_HEAP_TYPE type;
    ID3D12DescriptorHeap* heap;

    u32 num_free;
    u32* free_list;

    u64 stride;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_base;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_base;

    u32* generations;

    void init(Arena* arena, ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE heap_type, u32 count, bool shader_visible)
    {
        capacity = count;
        type = heap_type;

        D3D12_DESCRIPTOR_HEAP_DESC desc = {};
        desc.NumDescriptors = count;
        desc.Type = type;

        if (shader_visible) {
            desc.Flags |= D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        }

        device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap));

        free_list = arena->push_array<u32>(count);

        generations = arena->push_array<u32>(count);

        for (u32 i = 0; i < count; ++i) {
            free_list[num_free++] = i;
            generations[i] = 1;
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

    bool descriptor_valid(Descriptor desc) {
        return desc.index < capacity && desc.generation == generations[desc.index];
    }

    void validate_descriptor(Descriptor desc) {
        (void)desc;
        assert(descriptor_valid(desc));
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
        desc.generation = generations[desc.index];
        return desc;
    }

    Descriptor create_rtv(ID3D12Device* device, ID3D12Resource* resource, D3D12_RENDER_TARGET_VIEW_DESC* rtv_desc) {
        assert(type == D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        Descriptor d = alloc_descriptor();
        device->CreateRenderTargetView(resource, rtv_desc, cpu_handle(d));
        return d;
    }

    Descriptor create_srv(ID3D12Device* device, ID3D12Resource* resource, D3D12_SHADER_RESOURCE_VIEW_DESC* srv_desc) {
        assert(type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        Descriptor d = alloc_descriptor();
        device->CreateShaderResourceView(resource, srv_desc, cpu_handle(d));
        return d;
    }

    Descriptor create_uav(ID3D12Device* device, ID3D12Resource* resource, D3D12_UNORDERED_ACCESS_VIEW_DESC* uav_desc) {
        assert(type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        Descriptor d = alloc_descriptor();
        device->CreateUnorderedAccessView(resource, 0, uav_desc, cpu_handle(d));
        return d;
    }

    Descriptor create_cbv(ID3D12Device* device, D3D12_CONSTANT_BUFFER_VIEW_DESC* cbv_desc) {
        assert(type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        Descriptor d = alloc_descriptor();
        device->CreateConstantBufferView(cbv_desc, cpu_handle(d));
        return d;
    }

    Descriptor create_dsv(ID3D12Device* device, ID3D12Resource* resource, D3D12_DEPTH_STENCIL_VIEW_DESC* dsv_desc) {
        assert(type == D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
        Descriptor d = alloc_descriptor();
        device->CreateDepthStencilView(resource, dsv_desc, cpu_handle(d));
        return d;
    }

    void free_descriptor(Descriptor desc) {
        validate_descriptor(desc);
        generations[desc.index]++;
        free_list[num_free++] = desc.index;
    }
};

struct UploadPool {
    ID3D12Resource* buffer;
    void* ptr;
    u32 allocated;
    u32 size;
};

struct ConstantBuffer {
    Descriptor view;
    void* ptr;
};

struct UploadRegion {
    ID3D12Resource* resource;
    u32 offset;
};

struct CommandList {
    D3D12_COMMAND_LIST_TYPE type;
    ID3D12GraphicsCommandList* list;
    ID3D12CommandAllocator* allocator;
    Vec<ConstantBuffer> constant_buffers;
    Vec<UploadPool> upload_pools;
    u64 fence_val;

    ID3D12GraphicsCommandList* operator->() {
        return list;
    }

    UploadRegion get_upload_region(Renderer* r, u32 data_size, void* data);
    void buffer_upload(Renderer* renderer, ID3D12Resource* buffer, u32 data_size, void* data);

    void drop_constant_buffer(ConstantBuffer cbuffer) {
        constant_buffers.push(cbuffer);
    }
};

struct Queue {
    ID3D12CommandQueue* queue;
    u64 fence_val;
    ID3D12Fence* fence;
    Vec<CommandList> occupied_command_lists;

    void init(ID3D12Device* device, D3D12_COMMAND_LIST_TYPE type) {
        assert(!queue);

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

    void poll_command_lists(Vec<CommandList>* avail_lists, Vec<ConstantBuffer>* avail_cbuffers, Vec<UploadPool>* avail_upload_pools) {
        for (int i = occupied_command_lists.len-1; i >= 0; --i)
        {
            CommandList list = occupied_command_lists[i];

            if (reached(list.fence_val)) {
                for (u32 j = 0; j < list.constant_buffers.len; ++j) {
                    avail_cbuffers->push(list.constant_buffers[j]);
                }

                for (u32 j = 0; j < list.upload_pools.len; ++j) {
                    UploadPool upload_pool = list.upload_pools[j];
                    if (upload_pool.size > DEFAULT_UPLOAD_POOL_SIZE) {
                        upload_pool.buffer->Release();
                    }
                    else {
                        upload_pool.allocated = 0;
                        avail_upload_pools->push(upload_pool);
                    }
                }

                list.constant_buffers.clear();
                list.upload_pools.clear();

                avail_lists->push(list);

                occupied_command_lists.remove_by_patch(i);
            }
        }
    }
};

struct MeshData {
    ID3D12Resource* vbuffer;
    ID3D12Resource* ibuffer;
    Descriptor vbuffer_view;
    Descriptor ibuffer_view;
    u32 index_count;
};

struct TextureData {
    u32 width;
    u32 height;
    DXGI_FORMAT format;
    ID3D12Resource* resource;
    D3D12_RESOURCE_STATES state;
    Descriptor view;
    Descriptor rtv;
    Descriptor dsv;
    Descriptor uav;

    void transition(CommandList* cmd, D3D12_RESOURCE_STATES target_state) {
        if (state == target_state) {
            return;
        }

        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resource;
        barrier.Transition.StateBefore = state;
        barrier.Transition.StateAfter = target_state;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

        cmd->list->ResourceBarrier(1, &barrier);

        state = target_state;
    }
};

struct RDUploadContext {
    CommandList command_list;
};

struct RDUploadStatus {
};

template<typename DataType, typename HandleType>
struct HandledResourceManager {
    struct Node {
        DataType data;
        Node* next;
        u32 generation;
    };

    Arena* arena;
    Node* free_list;

    void init(Arena* arena_backing) {
        arena = arena_backing;
    }

    void validate_handle(HandleType handle) {
        (void)handle;
        assert(handle.data);
        assert(handle.generation == ((Node*)handle.data)->generation);
    }

    HandleType alloc() {
        Node* node = 0;

        if (!free_list) {
            node = arena->push_type<Node>();
            node->generation = 1;
        }
        else {
            node = free_list;
            free_list = node->next;
        }
        
        HandleType handle;
        handle.generation = node->generation;
        handle.data = (DataType*)node;

        return handle;
    }

    void free(HandleType handle) {
        validate_handle(handle);

        Node* node = (Node*)handle.data;

        memset(&node->data, 0, sizeof(node->data));
        node->generation++;

        node->next = free_list;
        free_list = node;
    }

    DataType* at(HandleType handle) {
        validate_handle(handle);
        return (DataType*)handle.data;
    }
};

struct Pipeline {
    bool is_compute;
    ID3D12PipelineState* pipeline_state;
    Dictionary<int> bindings;
    u32 group_size_x;
    u32 group_size_y;
    u32 group_size_z;

    void bind(CommandList* cmd) {
        cmd->list->SetPipelineState(pipeline_state);
    }

    void bind_descriptor_at_offset(CommandList* cmd, int offset, Descriptor descriptor) {
        if (is_compute) {
            cmd->list->SetComputeRoot32BitConstant(0, descriptor.index, offset);
        }
        else {
            cmd->list->SetGraphicsRoot32BitConstant(0, descriptor.index, offset);
        }
    }

    void bind_descriptor(CommandList* cmd, const char* name, Descriptor descriptor) {
        int offset = bindings[name];
        bind_descriptor_at_offset(cmd, offset, descriptor);
    }

    void free() {
        pipeline_state->Release();
        bindings.free();
    }
};

struct RenderGraph;

struct Renderer {
    Arena arena;
    HWND window;

    IDXGIFactory3* factory;
    IDXGIAdapter* adapter;
    ID3D12Device* device;

    Queue direct_queue;
    Queue copy_queue;

    DescriptorHeap rtv_heap;
    DescriptorHeap bindless_heap;
    DescriptorHeap dsv_heap;

    IDXGISwapChain3* swapchain;
    DXGI_FORMAT swapchain_format;
    u32 swapchain_buffer_count;
    u32 swapchain_w, swapchain_h;
    u64 swapchain_fences[DXGI_MAX_SWAP_CHAIN_BUFFERS];
    ID3D12Resource* swapchain_buffers[DXGI_MAX_SWAP_CHAIN_BUFFERS];

    Vec<ID3D12Resource*> permanent_resources;

    Vec<CommandList> available_command_lists;
    Vec<ConstantBuffer> available_constant_buffers;
    Vec<UploadPool> available_upload_pools;

    HandledResourceManager<MeshData, RDMesh> mesh_manager;
    HandledResourceManager<TextureData, RDTexture> texture_manager;
    
    PoolAllocator<RDUploadContext> upload_context_allocator;

    ID3D12RootSignature* root_signature;

    Pipeline gbuffer_pipeline;
    Pipeline lighting_pipeline;

    RenderGraph* render_graph;

    ID3D12Resource* point_light_buffer;
    ID3D12Resource* directional_light_buffer;
    Descriptor point_light_buffer_view;
    Descriptor directional_light_buffer_view;

    RDTexture white_texture;

    RDRenderInfo* render_info;
    XMMATRIX view_projection_matrix; 

    void get_swapchain_buffers() {
        for (u32 i = 0; i < swapchain_buffer_count; ++i) {
            swapchain->GetBuffer(i, IID_PPV_ARGS(&swapchain_buffers[i]));
        }
    }

    void release_swapchain_buffers() {
        for (u32 i = 0; i < swapchain_buffer_count; ++i) {
            swapchain_buffers[i]->Release();
        }
    }

    CommandList open_command_list(D3D12_COMMAND_LIST_TYPE type) {
        direct_queue.poll_command_lists(&available_command_lists, &available_constant_buffers, &available_upload_pools);
        copy_queue.poll_command_lists(&available_command_lists, &available_constant_buffers, &available_upload_pools);

        CommandList list = {};

        for (int i = available_command_lists.len - 1; i >= 0; --i) {
            if (available_command_lists[i].type == type) {
                list = available_command_lists[i];
                available_command_lists.remove_by_patch(i);
                break;
            }
        }

        if (!list.list) {
            list.type = type;
            device->CreateCommandAllocator(type, IID_PPV_ARGS(&list.allocator));
            device->CreateCommandList(0, type, list.allocator, 0, IID_PPV_ARGS(&list.list));
            list.list->Close();
        }

        list.allocator->Reset();
        list.list->Reset(list.allocator, 0);

        return list;
    }

    ConstantBuffer get_constant_buffer(u32 size, void* data) {
        assert(size <= CONSTANT_BUFFER_CAPACITY);

        if (available_constant_buffers.empty()) {
            D3D12_RESOURCE_DESC desc = {};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            desc.Width = CONSTANT_BUFFER_CAPACITY * CONSTANT_BUFFER_POOL_COUNT;
            desc.Height = 1;
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 1;
            desc.SampleDesc.Count = 1;
            desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

            D3D12_HEAP_PROPERTIES heap_properties = {};
            heap_properties.Type = D3D12_HEAP_TYPE_UPLOAD;

            ID3D12Resource* buf = 0;
            device->CreateCommittedResource(&heap_properties, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, 0, IID_PPV_ARGS(&buf));

            void* base;
            buf->Map(0, 0, &base);
            u64 gpu_base = buf->GetGPUVirtualAddress();

            for (int i = 0; i < CONSTANT_BUFFER_POOL_COUNT; ++i) {
                D3D12_CONSTANT_BUFFER_VIEW_DESC cbv_desc = {};
                cbv_desc.BufferLocation = gpu_base + i * CONSTANT_BUFFER_CAPACITY;
                cbv_desc.SizeInBytes = CONSTANT_BUFFER_CAPACITY;

                ConstantBuffer cbuffer = {};
                cbuffer.ptr = (u8*)base + i * CONSTANT_BUFFER_CAPACITY;
                cbuffer.view = bindless_heap.create_cbv(device, &cbv_desc);

                available_constant_buffers.push(cbuffer);
            }

            permanent_resources.push(buf);
        }

        ConstantBuffer cbuffer = available_constant_buffers.pop();
        memcpy(cbuffer.ptr, data, size);

        return cbuffer;
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

static ID3D12Resource* create_buffer(ID3D12Device* device, u32 size, D3D12_HEAP_TYPE heap_type, D3D12_RESOURCE_STATES initial_state) {
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    D3D12_HEAP_PROPERTIES heap_properties = {};
    heap_properties.Type = heap_type;

    ID3D12Resource* buffer = 0;
    device->CreateCommittedResource(&heap_properties, D3D12_HEAP_FLAG_NONE, &desc, initial_state, 0, IID_PPV_ARGS(&buffer));

    return buffer;
}

static UploadPool steal_suitable_upload_pool(Vec<UploadPool>* list, u32 size) {
    UploadPool found_pool = {};

    for (u32 i = 0; i < list->len; ++i)
    {
        UploadPool pool = list->at(i);

        if (pool.size-pool.allocated >= size) {
            found_pool = pool;
            list->remove_by_patch(i);
            break;
        }
    }

    return found_pool;
}

UploadRegion CommandList::get_upload_region(Renderer* r, u32 data_size, void* data) {
    UploadPool pool = steal_suitable_upload_pool(&upload_pools, data_size);

    if (!pool.ptr) {
        pool = steal_suitable_upload_pool(&r->available_upload_pools, data_size);
    }

    if (!pool.ptr) {
        u32 pool_size = max(data_size, DEFAULT_UPLOAD_POOL_SIZE);

        pool.buffer = create_buffer(r->device, pool_size, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        pool.buffer->Map(0, 0, &pool.ptr);
        pool.allocated = 0;
        pool.size = pool_size;
    }

    assert(pool.size-pool.allocated >= data_size);

    u32 offset = pool.allocated;
    memcpy((u8*)pool.ptr + offset, data, data_size);

    pool.allocated += data_size;
    upload_pools.push(pool);

    UploadRegion region = {};
    region.resource = pool.buffer;
    region.offset = offset;

    return region;
}

void CommandList::buffer_upload(Renderer* r, ID3D12Resource* buffer, u32 data_size, void* data) {
    UploadRegion region = get_upload_region(r, data_size, data);
    list->CopyBufferRegion(buffer, 0, region.resource, region.offset, data_size);
}

static void get_pipeline_reflection_data(Shader shader, Pipeline* pipeline) {
    IDxcUtils* utils;
    DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&utils));

    DxcBuffer shader_buf = {};
    shader_buf.Ptr = shader.memory;
    shader_buf.Size = shader.len;

    ID3D12ShaderReflection* reflection;
    utils->CreateReflection(&shader_buf, IID_PPV_ARGS(&reflection));

    reflection->GetThreadGroupSize(&pipeline->group_size_x, &pipeline->group_size_y, &pipeline->group_size_z);

    ID3D12ShaderReflectionConstantBuffer* cbuffer = reflection->GetConstantBufferByIndex(0);

    D3D12_SHADER_BUFFER_DESC cbuffer_desc;
    cbuffer->GetDesc(&cbuffer_desc);

    for (u32 i = 0; i < cbuffer_desc.Variables; ++i) {
        ID3D12ShaderReflectionVariable* var = cbuffer->GetVariableByIndex(i);
        D3D12_SHADER_VARIABLE_DESC var_desc;
        var->GetDesc(&var_desc);
        pipeline->bindings.insert(var_desc.Name, i);
    }

    utils->Release();
}

static Pipeline create_graphics_pipeline(ID3D12Device* device, ID3D12RootSignature* root_signature, u32 num_rtvs, DXGI_FORMAT* rtv_formats, const char* vs_ps_path) {
    Scratch scratch = get_scratch(0);

    Shader vs = compile_shader(scratch.arena, vs_ps_path, "vs_main", "vs_6_6");
    Shader ps = compile_shader(scratch.arena, vs_ps_path, "ps_main", "ps_6_6");

    assert(vs.len);
    assert(ps.len);

    Pipeline pipeline = {};
    get_pipeline_reflection_data(vs, &pipeline);

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline_state_desc = {};

    pipeline_state_desc.pRootSignature = root_signature;

    pipeline_state_desc.VS.BytecodeLength  = vs.len;
    pipeline_state_desc.VS.pShaderBytecode = vs.memory;
    pipeline_state_desc.PS.BytecodeLength  = ps.len;
    pipeline_state_desc.PS.pShaderBytecode = ps.memory;

    for (int i = 0; i < ARRAY_LEN(pipeline_state_desc.BlendState.RenderTarget); ++i) {
        D3D12_RENDER_TARGET_BLEND_DESC* blend = pipeline_state_desc.BlendState.RenderTarget + i;
        blend->SrcBlend = D3D12_BLEND_ONE;
        blend->DestBlend = D3D12_BLEND_ZERO;
        blend->BlendOp = D3D12_BLEND_OP_ADD;
        blend->SrcBlendAlpha = D3D12_BLEND_ONE;
        blend->DestBlendAlpha = D3D12_BLEND_ZERO;
        blend->BlendOpAlpha = D3D12_BLEND_OP_ADD;
        blend->LogicOp = D3D12_LOGIC_OP_NOOP;
        blend->RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }

    pipeline_state_desc.SampleMask = D3D12_DEFAULT_SAMPLE_MASK;

    pipeline_state_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pipeline_state_desc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    pipeline_state_desc.RasterizerState.DepthClipEnable = TRUE;
    pipeline_state_desc.RasterizerState.FrontCounterClockwise = TRUE;

    pipeline_state_desc.DepthStencilState.DepthEnable = true;
    pipeline_state_desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pipeline_state_desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER;

    pipeline_state_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;

    assert(num_rtvs < ARRAY_LEN(pipeline_state_desc.RTVFormats));
    pipeline_state_desc.NumRenderTargets = num_rtvs;
    memcpy(pipeline_state_desc.RTVFormats, rtv_formats, num_rtvs * sizeof(DXGI_FORMAT));

    pipeline_state_desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pipeline_state_desc.SampleDesc.Count = 1;

    device->CreateGraphicsPipelineState(&pipeline_state_desc, IID_PPV_ARGS(&pipeline.pipeline_state));

    return pipeline;
}

static Pipeline create_compute_pipeline(ID3D12Device* device, ID3D12RootSignature* root_signature, const char* cs_path) {
    Scratch scratch = get_scratch(0);

    Shader cs = compile_shader(scratch.arena, cs_path, "cs_main", "cs_6_6");
    assert(cs.len);

    Pipeline pipeline = {};
    pipeline.is_compute = true;
    get_pipeline_reflection_data(cs, &pipeline);

    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_state_desc = {};

    pipeline_state_desc.pRootSignature = root_signature;
    pipeline_state_desc.CS.BytecodeLength = cs.len;
    pipeline_state_desc.CS.pShaderBytecode = cs.memory;

    device->CreateComputePipelineState(&pipeline_state_desc, IID_PPV_ARGS(&pipeline.pipeline_state));

    return pipeline;
}

struct RenderGraphTexture {
    int index;
    int version;
};

struct BindPair {
    Descriptor descriptor;
    int offset;
};

struct RenderGraphNode {
    using Procedure = void (*)(Renderer*, CommandList*, Pipeline*);

    Pipeline* pipeline;
    Procedure procedure;
    bool visited;
    RenderGraph* graph;
    StaticVec<RenderGraphTexture, 16> reads;
    StaticVec<RenderGraphTexture, 16> writes;
    StaticSet<RenderGraphNode*, 16> parents;
    
    StaticVec<BindPair, 16> binds;
    StaticVec<RDTexture, 16> write_by_uav_textures;
    StaticVec<RDTexture, 16> render_targets;
    bool has_depth_buffer;
    RDTexture depth_buffer_texture;

    RenderGraphNode* read(Renderer* r, RenderGraphTexture texture, const char* where);
    void mark_write(RenderGraphTexture& texture);
    RenderGraphNode* write(Renderer* r, RenderGraphTexture& texture, const char* where);
    RenderGraphNode* render_target(Renderer* r, RenderGraphTexture& texture);
    RenderGraphNode* depth_buffer(Renderer* r, RenderGraphTexture& texture);
    void execute(Renderer* r, CommandList* cmd);
};

struct RenderGraph {
    bool is_built;
    StaticVec<RDTexture, 16> textures;
    StaticVec<RenderGraphNode, 16> nodes;
    HashMap<RenderGraphTexture, RenderGraphNode*> texture_owners;
    RenderGraphNode* final_node;
    StaticVec<RenderGraphNode*, 16> ordered_nodes;

    RenderGraphTexture create_texture(Renderer* r, RDFormat format, RDTextureUsage usage) {
        RDTexture texture = rd_create_texture(r, r->swapchain_w, r->swapchain_h, format, usage);
        textures.push(texture);

        RenderGraphTexture handle;
        handle.index = textures.len-1;
        handle.version = 0;

        return handle;
    }

    RenderGraphNode* add_pass(Pipeline* pipeline, RenderGraphNode::Procedure procedure) {
        RenderGraphNode node = {};
        node.pipeline = pipeline;
        node.procedure = procedure;
        node.graph = this;
        return &nodes.push(node);
    }

    void set_final_pass(RenderGraphNode* node) {
        final_node = node;
    }

    void visit_node(RenderGraphNode* node) {
        if (node->visited) {
            return;
        }

        auto fn = [=](RenderGraphNode* node) {
            visit_node(node);
        };

        node->visited = true;
        node->parents.for_each(fn);
        ordered_nodes.push(node);
    }

    void build() {
        assert(final_node && "must give final node");

        for (u32 i = 0; i < nodes.len; ++i) {
            RenderGraphNode* node = &nodes[i];
            
            for (u32 j = 0; j < node->reads.len; ++j) {
                if (node->reads[j].version != 0) {
                    RenderGraphNode* parent = texture_owners[node->reads[j]];
                    node->parents.insert(parent);
                }
            }
        }

        visit_node(final_node);

        is_built = true;
    }

    RDTexture execute(Renderer* r, CommandList* cmd) {
        for (u32 i = 0; i < ordered_nodes.len; ++i) {
            ordered_nodes[i]->execute(r, cmd);
        }
        
        return textures[final_node->writes[0].index];
    }

    void free(Renderer* r) {
        for (u32 i = 0; i < textures.len; ++i) {
            rd_free_texture(r, textures[i]);
        }
        
        texture_owners.free();

        memset(this, 0, sizeof(*this));
    }
};

RenderGraphNode* RenderGraphNode::read(Renderer* r, RenderGraphTexture texture, const char* where) {
    reads.push(texture);

    BindPair bind;
    bind.descriptor = r->texture_manager.at(graph->textures[texture.index])->view;
    bind.offset = pipeline->bindings[where];

    binds.push(bind);

    return this;
}

void RenderGraphNode::mark_write(RenderGraphTexture& texture) {
    texture.version++;
    graph->texture_owners.insert(texture, this);
    writes.push(texture);
}

RenderGraphNode* RenderGraphNode::write(Renderer* r, RenderGraphTexture& texture, const char* where) {
    mark_write(texture);

    BindPair bind;
    bind.descriptor = r->texture_manager.at(graph->textures[texture.index])->uav;
    bind.offset = pipeline->bindings[where];

    binds.push(bind);
    write_by_uav_textures.push(graph->textures[texture.index]);
    
    return this;
}

RenderGraphNode* RenderGraphNode::render_target(Renderer* r, RenderGraphTexture& texture) {
    (void)r;
    assert(!pipeline->is_compute);
    mark_write(texture);
    render_targets.push(graph->textures[texture.index]);
    return this;
}

RenderGraphNode* RenderGraphNode::depth_buffer(Renderer* r, RenderGraphTexture& texture) {
    (void)r;
    assert(!pipeline->is_compute);
    mark_write(texture);
    has_depth_buffer = true;
    depth_buffer_texture = graph->textures[texture.index];
    return this;
}

void RenderGraphNode::execute(Renderer* r, CommandList* cmd) {
    pipeline->bind(cmd);

    for (u32 i = 0; i < reads.len; ++i) {
        RDTexture texture = graph->textures[reads[i].index];
        r->texture_manager.at(texture)->transition(cmd, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    }

    for (u32 i = 0; i < write_by_uav_textures.len; ++i) {
        RDTexture texture = write_by_uav_textures[i];
        r->texture_manager.at(texture)->transition(cmd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    if (!pipeline->is_compute) {
        StaticVec<D3D12_CPU_DESCRIPTOR_HANDLE, 16> rtvs = {};
        D3D12_CPU_DESCRIPTOR_HANDLE dsv = {};

        for (u32 i = 0; i < render_targets.len; ++i) {
            TextureData* texture_data = r->texture_manager.at(render_targets[i]);
            texture_data->transition(cmd, D3D12_RESOURCE_STATE_RENDER_TARGET);

            f32 color[4] = {};
            rtvs.push(r->rtv_heap.cpu_handle(texture_data->rtv));
            cmd->list->ClearRenderTargetView(rtvs[i], color, 0, 0);
        }

        if (has_depth_buffer) {
            TextureData* texture_data = r->texture_manager.at(depth_buffer_texture);
            texture_data->transition(cmd, D3D12_RESOURCE_STATE_DEPTH_WRITE);
            dsv = r->dsv_heap.cpu_handle(texture_data->dsv);
            cmd->list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, 0);
            texture_data->transition(cmd, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        }

        cmd->list->OMSetRenderTargets(rtvs.len, rtvs.mem, 0, has_depth_buffer ? &dsv : 0);

        D3D12_VIEWPORT viewport = {};
        viewport.Width = (f32)r->swapchain_w;
        viewport.Height = (f32)r->swapchain_h;
        viewport.MaxDepth = 1.0f;

        cmd->list->RSSetViewports(1, &viewport);

        D3D12_RECT scissor = {};
        scissor.right = r->swapchain_w;
        scissor.bottom = r->swapchain_h;

        cmd->list->RSSetScissorRects(1, &scissor);
    }

    for (u32 i = 0; i < binds.len; ++i) {
        BindPair bind = binds[i];
        pipeline->bind_descriptor_at_offset(cmd, bind.offset, bind.descriptor);
    }

    procedure(r, cmd, pipeline);
}

Renderer* rd_init(Arena* arena, void* window) {
    Scratch scratch = get_scratch(arena);
    Renderer* r = arena->push_type<Renderer>();

    r->arena = arena->sub_arena(RENDERER_ARENA_SIZE);

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
    r->copy_queue.init(r->device, D3D12_COMMAND_LIST_TYPE_COPY);

    r->rtv_heap.init(arena, r->device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, MAX_RTV_COUNT, false);
    r->bindless_heap.init(arena, r->device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, MAX_CBV_SRV_UAV_COUNT, true);
    r->dsv_heap.init(arena, r->device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, MAX_DSV_COUNT, false);

    r->mesh_manager.init(&r->arena);
    r->texture_manager.init(&r->arena);

    r->upload_context_allocator.init(&r->arena); 

    RDUploadContext* upload_context = rd_open_upload_context(r);

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

    D3D12_ROOT_PARAMETER root_param = {};
    root_param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    root_param.Constants.Num32BitValues = 32;

    root_signature_desc.NumParameters = 1;
    root_signature_desc.pParameters = &root_param;

    D3D12_STATIC_SAMPLER_DESC static_samplers[1] = {};
    static_samplers[0].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    static_samplers[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    static_samplers[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    static_samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    static_samplers[0].MaxLOD = D3D12_FLOAT32_MAX;
    static_samplers[0].ShaderRegister = 0;
    static_samplers[0].RegisterSpace = 0;

    root_signature_desc.NumStaticSamplers = ARRAY_LEN(static_samplers);
    root_signature_desc.pStaticSamplers = static_samplers;

    root_signature_desc.Flags |= D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;

    ID3DBlob* root_signature_code;
    D3D12SerializeRootSignature(&root_signature_desc, D3D_ROOT_SIGNATURE_VERSION_1, &root_signature_code, 0);
    r->device->CreateRootSignature(0, root_signature_code->GetBufferPointer(), root_signature_code->GetBufferSize(), IID_PPV_ARGS(&r->root_signature));
    root_signature_code->Release();

    DXGI_FORMAT rtv_formats[] = {
        DXGI_FORMAT_R8G8B8A8_UNORM,
        DXGI_FORMAT_R8G8B8A8_UNORM,
    };

    r->gbuffer_pipeline = create_graphics_pipeline(
        r->device,
        r->root_signature,
        ARRAY_LEN(rtv_formats), rtv_formats,
        "shaders/gbuffer.hlsl"
    );

    r->lighting_pipeline = create_compute_pipeline(r->device, r->root_signature, "shaders/lighting.hlsl");

    r->point_light_buffer = create_buffer(r->device, MAX_POINT_LIGHT_COUNT * sizeof(RDPointLight), D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON);
    r->directional_light_buffer = create_buffer(r->device, MAX_DIRECTIONAL_LIGHT_COUNT * sizeof(RDDirectionalLight), D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON);
    
    D3D12_SHADER_RESOURCE_VIEW_DESC structured_buffer_view_desc = {};
    structured_buffer_view_desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    structured_buffer_view_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

    structured_buffer_view_desc.Buffer.NumElements = MAX_POINT_LIGHT_COUNT;
    structured_buffer_view_desc.Buffer.StructureByteStride = sizeof(RDPointLight);
    r->point_light_buffer_view = r->bindless_heap.create_srv(r->device, r->point_light_buffer, &structured_buffer_view_desc);

    structured_buffer_view_desc.Buffer.NumElements = MAX_DIRECTIONAL_LIGHT_COUNT;
    structured_buffer_view_desc.Buffer.StructureByteStride = sizeof(RDDirectionalLight);
    r->directional_light_buffer_view = r->bindless_heap.create_srv(r->device, r->directional_light_buffer, &structured_buffer_view_desc);

    u32 white_texture_data = UINT32_MAX;
    r->white_texture = rd_create_texture(r, 1, 1, RD_FORMAT_RGBA8_UNORM, RD_TEXTURE_USAGE_RESOURCE);
    rd_upload_texture_data(r, upload_context, r->white_texture, &white_texture_data);

    r->render_graph = arena->push_type<RenderGraph>();

    RDUploadStatus* upload_status = rd_submit_upload_context(r, upload_context);
    rd_flush_upload(r, upload_status); 

    return r;
}

void rd_free(Renderer* r) {
    r->direct_queue.flush();
    r->copy_queue.flush();

    r->direct_queue.poll_command_lists(&r->available_command_lists, &r->available_constant_buffers, &r->available_upload_pools);
    r->copy_queue.poll_command_lists(&r->available_command_lists, &r->available_constant_buffers, &r->available_upload_pools);

    for (u32 i = 0; i < r->available_command_lists.len; ++i) {
        r->available_command_lists[i].list->Release();
        r->available_command_lists[i].allocator->Release();
        r->available_command_lists[i].constant_buffers.free();
        r->available_command_lists[i].upload_pools.free();
    }

    for (u32 i = 0; i < r->available_upload_pools.len; ++i) {
        r->available_upload_pools[i].buffer->Release();
    }

    for (u32 i = 0; i < r->permanent_resources.len; ++i) {
        r->permanent_resources[i]->Release();
    }

    r->render_graph->free(r);

    rd_free_texture(r, r->white_texture);

    r->directional_light_buffer->Release();
    r->point_light_buffer->Release();

    r->lighting_pipeline.free();
    r->gbuffer_pipeline.free();

    r->root_signature->Release();

    r->dsv_heap.free();
    r->bindless_heap.free();
    r->rtv_heap.free();

    r->release_swapchain_buffers();
    r->swapchain->Release();

    r->copy_queue.free();
    r->direct_queue.free();

    r->device->Release();
    r->adapter->Release();
    r->factory->Release();

    r->available_command_lists.free();
    r->available_constant_buffers.free();
    r->available_upload_pools.free();
}

RDUploadContext* rd_open_upload_context(Renderer* r) {
    RDUploadContext* upload_context = r->upload_context_allocator.alloc();
    upload_context->command_list = r->open_command_list(D3D12_COMMAND_LIST_TYPE_COPY);
    return upload_context;
}

RDUploadStatus* rd_submit_upload_context(Renderer* r, RDUploadContext* upload_context) {
    r->copy_queue.submit_command_list(upload_context->command_list);
    u64 fence_val = r->copy_queue.signal();
    r->upload_context_allocator.free(upload_context);
    return (RDUploadStatus*)fence_val;
}

bool rd_upload_status_finished(Renderer* r, RDUploadStatus* upload_status) {
    return r->copy_queue.reached((u64)upload_status);
}

void rd_flush_upload(Renderer* r, RDUploadStatus* upload_status) {
    return r->copy_queue.wait((u64)upload_status);
}

RDMesh rd_create_mesh(Renderer* r, RDUploadContext* upload_context, RDVertex* vertex_data, u32 vertex_count, u32* index_data, u32 index_count) {
    RDMesh handle = r->mesh_manager.alloc();
    MeshData* data = r->mesh_manager.at(handle);

    u32 vertex_data_size = vertex_count * sizeof(vertex_data[0]);
    u32 index_data_size = index_count * sizeof(index_data[0]);

    data->vbuffer = create_buffer(r->device, vertex_data_size, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON); 
    data->ibuffer = create_buffer(r->device, index_data_size, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON);

    upload_context->command_list.buffer_upload(r, data->vbuffer, vertex_data_size, vertex_data);
    upload_context->command_list.buffer_upload(r, data->ibuffer, index_data_size, index_data);

    D3D12_SHADER_RESOURCE_VIEW_DESC vbuffer_view_desc = {};
    vbuffer_view_desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    vbuffer_view_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    vbuffer_view_desc.Buffer.NumElements = vertex_count;
    vbuffer_view_desc.Buffer.StructureByteStride = sizeof(RDVertex);

    D3D12_SHADER_RESOURCE_VIEW_DESC ibuffer_view_desc = {};
    ibuffer_view_desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    ibuffer_view_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    ibuffer_view_desc.Buffer.NumElements = index_count;
    ibuffer_view_desc.Buffer.StructureByteStride = sizeof(u32);

    data->vbuffer_view = r->bindless_heap.create_srv(r->device, data->vbuffer, &vbuffer_view_desc);
    data->ibuffer_view = r->bindless_heap.create_srv(r->device, data->ibuffer, &ibuffer_view_desc);

    data->index_count = index_count;

    return handle;
}

void rd_free_mesh(Renderer* r, RDMesh mesh) {
    r->copy_queue.flush();
    r->direct_queue.flush();

    MeshData* data = r->mesh_manager.at(mesh);

    r->bindless_heap.free_descriptor(data->vbuffer_view);
    r->bindless_heap.free_descriptor(data->ibuffer_view);
    data->vbuffer->Release();
    data->ibuffer->Release();

    r->mesh_manager.free(mesh);
}

static DXGI_FORMAT rd_format_to_dxgi_format(RDFormat format) {
    switch (format) {
        case RD_FORMAT_RGBA8_UNORM:
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        case RD_FORMAT_R32_FLOAT:
            return DXGI_FORMAT_R32_FLOAT;
    }
    
    assert(false);
    return DXGI_FORMAT_UNKNOWN;
}

static DXGI_FORMAT format_to_depth_format(DXGI_FORMAT format) {
    switch (format) {
        case DXGI_FORMAT_R32_FLOAT:
            return DXGI_FORMAT_D32_FLOAT;
    }

    assert(false);
    return DXGI_FORMAT_UNKNOWN;
}

RDTexture rd_create_texture(Renderer* r, u32 width, u32 height, RDFormat format, RDTextureUsage usage) {
    RDTexture handle = r->texture_manager.alloc();
    TextureData* data = r->texture_manager.at(handle);

    data->width = width;
    data->height = height;
    data->format = rd_format_to_dxgi_format(format);

    D3D12_RESOURCE_DESC resource_desc = {};
    resource_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resource_desc.Width = width;
    resource_desc.Height = height;
    resource_desc.DepthOrArraySize = 1;
    resource_desc.MipLevels = 1;
    resource_desc.Format = data->format;
    resource_desc.SampleDesc.Count = 1;

    D3D12_RESOURCE_STATES initial_state = {};

    switch (usage) {
        default:
            assert(false);

        case RD_TEXTURE_USAGE_RESOURCE:
            initial_state = D3D12_RESOURCE_STATE_COMMON;
            break;

        case RD_TEXTURE_USAGE_RENDER_TARGET:
            initial_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
            resource_desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            break;

        case RD_TEXTURE_USAGE_DEPTH_BUFFER:
            initial_state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
            resource_desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
            break;
    }

    D3D12_HEAP_PROPERTIES texture_heap_props = {};
    texture_heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;

    r->device->CreateCommittedResource(&texture_heap_props, D3D12_HEAP_FLAG_NONE, &resource_desc, initial_state, 0, IID_PPV_ARGS(&data->resource));
    data->state = initial_state;

    D3D12_SHADER_RESOURCE_VIEW_DESC view_desc = {};
    view_desc.Format = resource_desc.Format;
    view_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    view_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view_desc.Texture2D.MipLevels = 1;

    data->view = r->bindless_heap.create_srv(r->device, data->resource, &view_desc);

    if (usage == RD_TEXTURE_USAGE_RENDER_TARGET) {
        D3D12_RENDER_TARGET_VIEW_DESC rtv_desc = {};
        rtv_desc.Format = resource_desc.Format;
        rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        data->rtv = r->rtv_heap.create_rtv(r->device, data->resource, &rtv_desc);

        D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc = {};
        uav_desc.Format = resource_desc.Format;
        uav_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        data->uav = r->bindless_heap.create_uav(r->device, data->resource, &uav_desc);
    }

    if (usage == RD_TEXTURE_USAGE_DEPTH_BUFFER) {
        D3D12_DEPTH_STENCIL_VIEW_DESC dsv_desc = {};
        dsv_desc.Format = format_to_depth_format(resource_desc.Format);
        dsv_desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        data->dsv = r->dsv_heap.create_dsv(r->device, data->resource, &dsv_desc);
    }

    return handle;
}

void rd_upload_texture_data(Renderer* r, RDUploadContext* upload_context, RDTexture texture, void* data) {
    TextureData* texture_data = r->texture_manager.at(texture);

    UploadRegion texture_upload_region = upload_context->command_list.get_upload_region(r, texture_data->width * texture_data->height * 4, data);

    D3D12_TEXTURE_COPY_LOCATION texture_copy_src = {};
    texture_copy_src.pResource = texture_upload_region.resource;
    texture_copy_src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    texture_copy_src.PlacedFootprint.Offset = texture_upload_region.offset;
    texture_copy_src.PlacedFootprint.Footprint.Format = texture_data->format;
    texture_copy_src.PlacedFootprint.Footprint.Width = texture_data->width;
    texture_copy_src.PlacedFootprint.Footprint.Height = texture_data->height;
    texture_copy_src.PlacedFootprint.Footprint.Depth = 1;
    texture_copy_src.PlacedFootprint.Footprint.RowPitch = texture_data->width * 4;

    D3D12_TEXTURE_COPY_LOCATION texture_copy_dst = {};
    texture_copy_dst.pResource = texture_data->resource;
    texture_copy_dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    texture_copy_dst.SubresourceIndex = 0;
   
    upload_context->command_list->CopyTextureRegion(&texture_copy_dst, 0, 0, 0, &texture_copy_src, 0);
}

void rd_free_texture(Renderer* r, RDTexture texture) {
    r->copy_queue.flush();
    r->direct_queue.flush();

    TextureData* data = r->texture_manager.at(texture);

    if (r->rtv_heap.descriptor_valid(data->rtv)) {
        r->rtv_heap.free_descriptor(data->rtv);
    }

    if (r->dsv_heap.descriptor_valid(data->dsv)) {
        r->dsv_heap.free_descriptor(data->dsv);
    }

    if (r->bindless_heap.descriptor_valid(data->uav)) {
        r->bindless_heap.free_descriptor(data->uav);
    }

    r->bindless_heap.free_descriptor(data->view);
    data->resource->Release();

    r->texture_manager.free(texture);
}

RDTexture rd_get_white_texture(Renderer* r) {
    return r->white_texture;
}

struct ShaderLightsInfo {
	u32 num_point_lights;
	u32 num_directional_lights;
	u32 point_lights_addr;
	u32 directional_lights_addr;
};

struct ShaderMaterial {
    u32 albedo_texture_addr;
    XMFLOAT3 albedo_factor;
};

static void gbuffer_pass_proc(Renderer* r, CommandList* cmd, Pipeline* pipeline) {
    cmd->list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    ConstantBuffer camera_cbuffer = r->get_constant_buffer(sizeof(XMMATRIX), &r->view_projection_matrix);
    cmd->drop_constant_buffer(camera_cbuffer);
    pipeline->bind_descriptor(cmd, "camera_addr", camera_cbuffer.view);

    int vbuffer_addr   = pipeline->bindings["vbuffer_addr"];
    int ibuffer_addr   = pipeline->bindings["ibuffer_addr"];
    int transform_addr = pipeline->bindings["transform_addr"];
    int material_addr  = pipeline->bindings["material_addr"];

    for (u32 i = 0; i < r->render_info->num_instances; ++i) {
        RDMeshInstance instance = r->render_info->instances[i];

        MeshData* mesh_data = r->mesh_manager.at(instance.mesh);
        TextureData* texture_data = r->texture_manager.at(instance.material.albedo_texture);

        ConstantBuffer transform_cbuffer = r->get_constant_buffer(sizeof(XMMATRIX), &instance.transform);
        cmd->drop_constant_buffer(transform_cbuffer);

        ShaderMaterial material;
        material.albedo_texture_addr = texture_data->view.index;
        material.albedo_factor = instance.material.albedo_factor;

        ConstantBuffer material_cbuffer = r->get_constant_buffer(sizeof(material), &material);
        cmd->drop_constant_buffer(material_cbuffer);

        pipeline->bind_descriptor_at_offset(cmd, vbuffer_addr  , mesh_data->vbuffer_view);
        pipeline->bind_descriptor_at_offset(cmd, ibuffer_addr  , mesh_data->ibuffer_view);
        pipeline->bind_descriptor_at_offset(cmd, transform_addr, transform_cbuffer.view);
        pipeline->bind_descriptor_at_offset(cmd, material_addr , material_cbuffer.view);

        cmd->list->DrawInstanced(mesh_data->index_count, 1, 0, 0);
    }
}

static void lighting_pass_proc(Renderer* r, CommandList* cmd, Pipeline* pipeline) {
    auto render_info = r->render_info;

    assert(render_info->num_point_lights <= MAX_POINT_LIGHT_COUNT);
    assert(render_info->num_directional_lights <= MAX_DIRECTIONAL_LIGHT_COUNT);

    cmd->buffer_upload(r, r->point_light_buffer, render_info->num_point_lights * sizeof(RDPointLight), render_info->point_lights);
    cmd->buffer_upload(r, r->directional_light_buffer, render_info->num_directional_lights * sizeof(RDDirectionalLight), render_info->directional_lights);

    ShaderLightsInfo lights_info = {};
    lights_info.num_point_lights = render_info->num_point_lights;
    lights_info.num_directional_lights = render_info->num_directional_lights;
    lights_info.point_lights_addr = r->point_light_buffer_view.index;
    lights_info.directional_lights_addr = r->directional_light_buffer_view.index;

    ConstantBuffer lights_cbuffer = r->get_constant_buffer(sizeof(lights_info), &lights_info);
    cmd->drop_constant_buffer(lights_cbuffer);
    pipeline->bind_descriptor(cmd, "lights_info_addr", lights_cbuffer.view);

    XMMATRIX inverse_view_projection_matrix = XMMatrixInverse(0, r->view_projection_matrix);
    ConstantBuffer inverse_view_projection_cbuffer = r->get_constant_buffer(sizeof(inverse_view_projection_matrix), &inverse_view_projection_matrix);
    cmd->drop_constant_buffer(inverse_view_projection_cbuffer);
    pipeline->bind_descriptor(cmd, "inverse_view_projection_addr", inverse_view_projection_cbuffer.view);
    
    cmd->list->Dispatch(r->swapchain_w / pipeline->group_size_x + 1, r->swapchain_h / pipeline->group_size_y + 1, 1);
}

void rd_render(Renderer* r, RDRenderInfo* render_info) {
    r->render_info = render_info;

    auto [window_w, window_h] = hwnd_size(r->window);

    if (window_w == 0 || window_h == 0) {
        return;
    }

    if (r->swapchain_w != window_w || r->swapchain_h != window_h) {
        r->direct_queue.flush();

        r->render_graph->free(r);

        r->release_swapchain_buffers();
        r->swapchain->ResizeBuffers(0, window_w, window_h, DXGI_FORMAT_UNKNOWN, 0);

        r->swapchain_w = window_w;
        r->swapchain_h = window_h;

        r->get_swapchain_buffers();
    }

    if (!r->render_graph->is_built) {
        RenderGraphTexture gbuffer_albedo = r->render_graph->create_texture(r, RD_FORMAT_RGBA8_UNORM, RD_TEXTURE_USAGE_RENDER_TARGET);
        RenderGraphTexture gbuffer_normal = r->render_graph->create_texture(r, RD_FORMAT_RGBA8_UNORM, RD_TEXTURE_USAGE_RENDER_TARGET);
        RenderGraphTexture render_target2 = r->render_graph->create_texture(r, RD_FORMAT_RGBA8_UNORM, RD_TEXTURE_USAGE_RENDER_TARGET);
        RenderGraphTexture depth_buffer = r->render_graph->create_texture(r, RD_FORMAT_R32_FLOAT, RD_TEXTURE_USAGE_DEPTH_BUFFER);

        r->render_graph->add_pass(&r->gbuffer_pipeline, gbuffer_pass_proc)
            ->render_target(r, gbuffer_albedo)
            ->render_target(r, gbuffer_normal)
            ->depth_buffer(r, depth_buffer);

        auto final_pass = r->render_graph->add_pass(&r->lighting_pipeline, lighting_pass_proc)
            ->write(r, render_target2, "target_texture_addr")
            ->read(r, gbuffer_albedo, "albedo_texture_addr")
            ->read(r, gbuffer_normal, "normal_texture_addr")
            ->read(r, depth_buffer, "depth_texture_addr");

        r->render_graph->set_final_pass(final_pass);

        r->render_graph->build();
    }

    u32 swapchain_index = r->swapchain->GetCurrentBackBufferIndex();
    r->direct_queue.wait(r->swapchain_fences[swapchain_index]);

    CommandList cmd = r->open_command_list(D3D12_COMMAND_LIST_TYPE_DIRECT);
    cmd->SetGraphicsRootSignature(r->root_signature);
    cmd->SetComputeRootSignature(r->root_signature);
    cmd->SetDescriptorHeaps(1, &r->bindless_heap.heap);

    XMMATRIX view_matrix = XMMatrixInverse(0, render_info->camera->transform);
    XMMATRIX projection_matrix = XMMatrixPerspectiveFovRH(render_info->camera->vertical_fov, (f32)r->swapchain_w/(f32)r->swapchain_h, 1000.0f, 0.1f);
    r->view_projection_matrix = view_matrix * projection_matrix;

    RDTexture final_image = r->render_graph->execute(r, &cmd);

    r->texture_manager.at(final_image)->transition(&cmd, D3D12_RESOURCE_STATE_COPY_SOURCE);

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = r->swapchain_buffers[swapchain_index];
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    cmd->ResourceBarrier(1, &barrier);

    D3D12_TEXTURE_COPY_LOCATION blit_src = {};
    blit_src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    blit_src.pResource = r->texture_manager.at(final_image)->resource;
    blit_src.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION blit_dst = {};
    blit_dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    blit_dst.pResource = r->swapchain_buffers[swapchain_index];
    blit_dst.SubresourceIndex = 0;

    cmd->CopyTextureRegion(&blit_dst, 0, 0, 0, &blit_src, 0);

    swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    cmd->ResourceBarrier(1, &barrier);

    r->direct_queue.submit_command_list(cmd);

    r->swapchain->Present(0, 0);
    r->swapchain_fences[swapchain_index] = r->direct_queue.signal();
}
