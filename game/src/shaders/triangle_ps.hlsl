#include "triangle_common.hlsli"

float4 main(VSOut surface) : SV_target
{
	float3 normal = normalize(surface.normal);

	float3 light_pos = float3(5.0f, 5.0f, 5.0f);
	float3 light_dir = normalize(light_pos - surface.world_space_pos);

	float3 diffuse_factor = 0.5f.xxx;
	float3 diffuse_contribution = max(dot(normal, light_dir), 0.0f) * diffuse_factor;
	float3 ambient_contribution = 0.01f.xxx;

	float3 lighting = diffuse_contribution + ambient_contribution;

	return float4(sqrt(lighting), 1.0f);
}