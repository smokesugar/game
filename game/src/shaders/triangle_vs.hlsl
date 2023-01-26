#include "triangle_common.hlsli"

struct Vertex {
    float3 pos;
    float3 norm;
    float2 uv;
};

static Vertex vbuffer[] = {
    { { 0.0f,  0.5f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f} },
    { {-0.5f, -0.5f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f} },
    { { 0.5f, -0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f} },
};

VSOut vs_main(uint vertex_id : SV_VertexID) {
    Vertex vertex = vbuffer[vertex_id];

    VSOut vso;
    vso.sv_pos = float4(vertex.pos, 1.0f);
    vso.color = vertex.norm;

    return vso;
}