#include "triangle_common.hlsli"

float4 main(VSOut vso) : SV_target
{
	return float4(sqrt(vso.color), 1.0f);
}