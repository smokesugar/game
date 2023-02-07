#include "common.hlsl"

struct Vertex {
    float3 pos;
    float3 norm;
    float2 uv;
};

cbuffer Constants : register(b0, space0)
{
    uint camera_addr;
    uint vbuffer_addr;
    uint ibuffer_addr;
    uint transform_addr;
    uint material_addr;
}

struct VSOut {
    float4 sv_pos : SV_Position;
    float3 normal : Normal;
    float2 uv : UV;
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
    vso.normal = normalize(mul((float3x3)transform.m, vertex.norm));
    vso.uv = vertex.uv;

    return vso;
}

struct Material {
	uint albedo_texture_addr;
	float3 albedo_factor;
};

struct PSOut {
	float4 albedo : SV_Target0;
    float4 normal : SV_Target1;
};

PSOut ps_main(VSOut surface)
{
	ConstantBuffer<Material> material = ResourceDescriptorHeap[material_addr];
	Texture2D<float3> albedo_texture = ResourceDescriptorHeap[material.albedo_texture_addr];
	float3 albedo = material.albedo_factor * pow(albedo_texture.Sample(linear_wrap_sampler, surface.uv), 2.0f);

    PSOut pso;
    pso.albedo = float4(albedo, 0.0f);
    pso.normal = float4(normalize(surface.normal) * 0.5f + 0.5f, 0.0f);

    return pso;
}
