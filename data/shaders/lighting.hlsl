#include "common.hlsl"

cbuffer RootConstants : register(b0, space0)
{
    uint albedo_texture_addr;
    uint normal_texture_addr;
    uint depth_texture_addr;
    uint target_texture_addr;
    uint lights_info_addr;
    uint inverse_view_projection_addr;
};

struct DirectionalLight {
	float3 direction;
	float3 intensity;
};

struct PointLight {
	float3 position;
	float3 intensity;
};

struct LightsInfo {
	uint num_point_lights;
	uint num_directional_lights;
	uint point_lights_addr;
	uint directional_lights_addr;
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

[numthreads(16, 16, 1)]
void cs_main(uint3 thread_id : SV_DispatchThreadID)
{
    RWTexture2D<float3> target_texture = ResourceDescriptorHeap[target_texture_addr]; 

    uint output_width, output_height;
    target_texture.GetDimensions(output_width, output_height);

    if (thread_id.x < output_width && thread_id.y < output_height) {
        Texture2D<float3> albedo_texture = ResourceDescriptorHeap[albedo_texture_addr];
        Texture2D<float3> normal_texture = ResourceDescriptorHeap[normal_texture_addr];
        Texture2D<float> depth_texture = ResourceDescriptorHeap[depth_texture_addr];
        ConstantBuffer<Matrix> inverse_view_projection = ResourceDescriptorHeap[inverse_view_projection_addr];

        ConstantBuffer<LightsInfo> lights_info = ResourceDescriptorHeap[lights_info_addr];
        StructuredBuffer<PointLight> point_lights = ResourceDescriptorHeap[lights_info.point_lights_addr];
        StructuredBuffer<DirectionalLight> directional_lights = ResourceDescriptorHeap[lights_info.directional_lights_addr];

        float3 albedo = albedo_texture[thread_id.xy];
        float depth = depth_texture[thread_id.xy];

        float multiplier = (float)(depth > 0.0f);
        
        float3 normal = normalize(normal_texture[thread_id.xy] * 2.0f - 1.0f);

        float4 position_screen_space = float4(
            ((float)thread_id.x/(float)output_width) * 2.0f - 1.0f,
            -(((float)thread_id.y/(float)output_height) * 2.0f - 1.0f),
            depth,
            1.0f
        );

        float4 position_world_space = mul(inverse_view_projection.m, position_screen_space);
        position_world_space /= position_world_space.w;

        float3 diffuse_light = 0.0f.xxx;

        for (uint i = 0; i < lights_info.num_point_lights; ++i) {
            PointLight light = point_lights[i];

            float3 to_light = light.position - position_world_space.xyz;		
            float distance = length(to_light);

            float distance_to_cull = max(light.intensity.x, max(light.intensity.y, light.intensity.z)) / 0.01f;
            
            if (distance < distance_to_cull) {
                float3 light_dir = to_light/distance; 
                float attenuation = 1.0f / distance;
                diffuse_light += max(dot(light_dir, normal), 0.0f) * light.intensity * attenuation;
            }
        }

        for (i = 0; i < lights_info.num_directional_lights; ++i) {
            DirectionalLight light = directional_lights[i];
            float3 light_dir = normalize(light.direction);		
            diffuse_light += max(dot(light_dir, normal), 0.0f) * light.intensity;
        }

        float3 ambient_light = 0.01f.xxx;
        float3 hdr = albedo * (diffuse_light + ambient_light);
        float3 ldr = ACESFilm(hdr);
        float3 tonemapped = sqrt(ldr);

        target_texture[thread_id.xy] = multiplier * tonemapped;
    }
}
