#include "triangle_common.hlsli"

struct Vertex {
    float3 pos;
    float3 norm;
    float2 uv;
};

VSOut vs_main(uint vertex_id : SV_VertexID) {
    StructuredBuffer<Vertex> vbuffer = ResourceDescriptorHeap[vbuffer_addr];
    StructuredBuffer<uint> ibuffer = ResourceDescriptorHeap[ibuffer_addr];

    Vertex vertex = vbuffer[ibuffer[vertex_id]];

    VSOut vso;
    vso.sv_pos = float4(vertex.pos, 1.0f);
    vso.color = vertex.norm;

    return vso;
}