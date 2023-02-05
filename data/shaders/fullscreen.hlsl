cbuffer RootConstants : register(b0, space0)
{
    uint source_texture_addr;
    uint target_texture_addr;
};

[numthreads(16, 16, 1)]
void cs_main(uint3 thread_id : SV_DispatchThreadID)
{
    Texture2D<float3> source_texture = ResourceDescriptorHeap[source_texture_addr];
    RWTexture2D<float3> target_texture = ResourceDescriptorHeap[target_texture_addr]; 

    uint width, height;
    target_texture.GetDimensions(width, height);

    if (thread_id.x < width && thread_id.y < height) {
        target_texture[thread_id.xy] = 1.0f.xxx - source_texture[thread_id.xy];
    }
}