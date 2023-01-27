struct VSOut {
    float4 sv_pos : SV_Position;
    float3 color : Color;
};

cbuffer Constants : register(b0, space0)
{
    uint camera_addr;
    uint vbuffer_addr;
    uint ibuffer_addr;
    uint transform_addr;
}
