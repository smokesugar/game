struct VSOut {
    float4 sv_pos : SV_Position;
    float3 world_space_pos : WorldSpacePos;
    float3 normal : Normal;
    float2 uv : UV;
};

cbuffer Constants : register(b0, space0)
{
    uint camera_addr;
    uint lighting_info_addr;
    uint vbuffer_addr;
    uint ibuffer_addr;
    uint transform_addr;
    uint texture_addr;
}
