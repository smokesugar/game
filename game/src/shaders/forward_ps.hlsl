#include "forward_common.hlsli"

struct DirectionalLight {
	float3 direction;
	float3 intensity;
};

struct PointLight {
	float3 position;
	float3 intensity;
};

struct LightingInfo {
	uint num_point_lights;
	uint num_directional_lights;
	uint directional_lights_addr;
	uint point_lights_addr;
};

float4 main(VSOut surface) : SV_target
{
	ConstantBuffer<LightingInfo> lighting_info = ResourceDescriptorHeap[lighting_info_addr];

	float3 normal = normalize(surface.normal);

	float3 light_dir = normalize(float3(5.0f, 5.0f, 5.0f));

	float3 diffuse_factor = 0.5f.xxx;
	float3 diffuse_contribution = max(dot(normal, light_dir), 0.0f) * diffuse_factor;
	float3 ambient_contribution = 0.01f.xxx;

	float3 lighting = diffuse_contribution + ambient_contribution;

	return float4(sqrt(lighting), 1.0f);
}