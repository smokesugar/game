#include "triangle_common.hlsli"

float4 main(VSOut vso) : SV_target
{
	return float4(normalize(vso.normal) * 0.5f + 0.5f, 1.0f);
}