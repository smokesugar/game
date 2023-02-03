#include "forward_common.hlsli"
#include "samplers.hlsli"

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
	uint point_lights_addr;
	uint directional_lights_addr;
};

struct Material {
	uint albedo_texture_addr;
	float3 albedo_factor;
};

float3 ACESFilm(float3 x)
{
    float a = 2.51f;
    float b = 0.03f;
    float c = 2.43f;
    float d = 0.59f;
    float e = 0.14f;
    return saturate((x*(a*x+b))/(x*(c*x+d)+e));
}

float4 main(VSOut surface) : SV_target
{
	ConstantBuffer<LightingInfo> lighting_info = ResourceDescriptorHeap[lighting_info_addr];
	StructuredBuffer<PointLight> point_lights = ResourceDescriptorHeap[lighting_info.point_lights_addr];
	StructuredBuffer<DirectionalLight> directional_lights = ResourceDescriptorHeap[lighting_info.directional_lights_addr];

	ConstantBuffer<Material> material = ResourceDescriptorHeap[material_addr];
	Texture2D<float3> albedo_texture = ResourceDescriptorHeap[material.albedo_texture_addr];

	float3 normal = normalize(surface.normal);

	float3 diffuse_light = 0.0f.xxx;

	for (uint i = 0; i < lighting_info.num_point_lights; ++i) {
		PointLight light = point_lights[i];

		float3 surface_to_light = light.position - surface.world_space_pos;		
		float distance = length(surface_to_light);

		float distance_to_cull = max(light.intensity.x, max(light.intensity.y, light.intensity.z)) / 0.01f;
		
		if (distance < distance_to_cull) {
            float3 light_dir = surface_to_light/distance; 
            float attenuation = 1.0f / distance;
            diffuse_light += max(dot(light_dir, normal), 0.0f) * light.intensity * attenuation;
		}
	}

	for (i = 0; i < lighting_info.num_directional_lights; ++i) {
		DirectionalLight light = directional_lights[i];
		float3 light_dir = normalize(light.direction);		
		diffuse_light += max(dot(light_dir, normal), 0.0f) * light.intensity;
	}

	float3 ambient_light = 0.01f.xxx;

	float3 albedo = material.albedo_factor * pow(albedo_texture.Sample(linear_wrap_sampler, surface.uv), 2.0f);

	float3 hdr = albedo * (diffuse_light + ambient_light);
	float3 ldr = ACESFilm(hdr);

	return float4(sqrt(ldr), 1.0f);
}