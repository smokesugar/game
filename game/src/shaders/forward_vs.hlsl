#include "forward_common.hlsli"

struct Vertex {
    float3 pos;
    float3 norm;
    float2 uv;
};

struct Matrix {
    float4x4 m;
};

VSOut vs_main(uint vertex_id : SV_VertexID) {
    ConstantBuffer<Matrix> camera = ResourceDescriptorHeap[camera_addr];
    StructuredBuffer<Vertex> vbuffer = ResourceDescriptorHeap[vbuffer_addr];
    StructuredBuffer<uint> ibuffer = ResourceDescriptorHeap[ibuffer_addr];
    ConstantBuffer<Matrix> transform = ResourceDescriptorHeap[transform_addr];

    Vertex vertex = vbuffer[ibuffer[vertex_id]];

    float4 world_space_pos = mul(transform.m, float4(vertex.pos, 1.0f));

    VSOut vso;
    vso.sv_pos = mul(camera.m, world_space_pos);
    vso.world_space_pos = world_space_pos.xyz;
    vso.normal = normalize(mul((float3x3)transform.m, vertex.norm));

    return vso;
}