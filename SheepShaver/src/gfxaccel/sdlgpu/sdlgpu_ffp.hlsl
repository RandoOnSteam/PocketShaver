struct VSInput
{
	float4 position : TEXCOORD0;
	float4 color : TEXCOORD1;
	float4 secondary : TEXCOORD2;
	float4 tex0 : TEXCOORD3;
	float4 tex1 : TEXCOORD4;
	float fogCoord : TEXCOORD5;
	float4 eye : TEXCOORD6;
};

struct VSOutput
{
	float4 position : SV_Position;
	float4 color : TEXCOORD0;
	float4 secondary : TEXCOORD1;
	float4 tex0 : TEXCOORD2;
	float4 tex1 : TEXCOORD3;
	float fogCoord : TEXCOORD4;
	float4 eye : TEXCOORD5;
#if defined(SPIRV)
	[[vk::builtin("PointSize")]]
#endif
	float pointSize : PSIZE;
};

VSOutput VSMain(VSInput input)
{
	VSOutput output;
	output.position = input.position;
	output.color = input.color;
	output.secondary = input.secondary;
	output.tex0 = input.tex0;
	output.tex1 = input.tex1;
	output.fogCoord = input.fogCoord;
	output.eye = input.eye;
	output.pointSize = 1.0;
	return output;
}

Texture2D Texture0 : register(t0, space2);
SamplerState Sampler0 : register(s0, space2);
Texture2D Texture1 : register(t1, space2);
SamplerState Sampler1 : register(s1, space2);
Texture3D Texture3 : register(t2, space2);
SamplerState Sampler3 : register(s2, space2);

cbuffer FragmentData : register(b0, space3)
{
	float4 U[24];
};

float4 SourceValue(int source, float4 texel, float4 primary,
	float4 previous, float4 constantColor)
{
	if (source == 5890)
		return texel;
	if (source == 34167)
		return primary;
	if (source == 34168)
		return previous;
	return constantColor;
}

float3 OperandRGB(float4 value, int operand)
{
	if (operand == 769)
		return 1.0 - value.rgb;
	if (operand == 770)
		return value.aaa;
	if (operand == 771)
		return 1.0 - value.aaa;
	return value.rgb;
}

float OperandAlpha(float4 value, int operand)
{
	if (operand == 771)
		return 1.0 - value.a;
	return value.a;
}

float4 CombineUnit(float4 previous, float4 primary, float4 texel,
	float4 constantColor, float4 flags, float4 combine, float4 source0,
	float4 source1, float4 source2)
{
	int mode = (int)flags.z;
	if (mode == 7681)
		return texel;
	if (mode == 8448)
		return previous * texel;
	if (mode == 260)
		return saturate(previous + texel);
	if (mode == 8449)
		return float4(lerp(previous.rgb, texel.rgb, texel.a), previous.a);
	if (mode != 34160)
		return previous * texel;
	float4 a = SourceValue((int)source0.x, texel, primary, previous, constantColor);
	float4 b = SourceValue((int)source0.y, texel, primary, previous, constantColor);
	float4 c = SourceValue((int)source0.z, texel, primary, previous, constantColor);
	float3 ar = OperandRGB(a, (int)source1.y);
	float3 br = OperandRGB(b, (int)source1.z);
	float3 cr = OperandRGB(c, (int)source1.w);
	float3 rgb;
	int rgbMode = (int)flags.w;
	if (rgbMode == 7681)
		rgb = ar;
	else if (rgbMode == 260)
		rgb = ar + br;
	else if (rgbMode == 34165)
		rgb = ar * cr + br * (1.0 - cr);
	else
		rgb = ar * br;
	float4 aa = SourceValue((int)source0.w, texel, primary, previous, constantColor);
	float4 ab = SourceValue((int)source1.x, texel, primary, previous, constantColor);
	float alphaA = OperandAlpha(aa, (int)source2.x);
	float alphaB = OperandAlpha(ab, (int)source2.y);
	float alpha;
	int alphaMode = (int)combine.x;
	if (alphaMode == 7681)
		alpha = alphaA;
	else if (alphaMode == 260)
		alpha = alphaA + alphaB;
	else
		alpha = alphaA * alphaB;
	return saturate(float4(rgb * combine.y, alpha * combine.z));
}

bool AlphaPass(float alpha, int func, float refValue)
{
	if (func == 512)
		return false;
	if (func == 513)
		return alpha < refValue;
	if (func == 514)
		return alpha == refValue;
	if (func == 515)
		return alpha <= refValue;
	if (func == 516)
		return alpha > refValue;
	if (func == 517)
		return alpha != refValue;
	if (func == 518)
		return alpha >= refValue;
	return true;
}

float4 PSMain(VSOutput input) : SV_Target0
{
	uint clipMask = (uint)U[4].z;
	for (int plane = 0; plane < 6; plane++) {
		if ((clipMask & (1u << plane)) != 0 && dot(input.eye, U[16 + plane]) < 0.0)
			discard;
	}
	float4 primary = input.color;
	float4 color = primary;
	float4 texel0 = float4(1, 1, 1, 1);
	if (U[5].x != 0.0)
		texel0 = Texture0.Sample(Sampler0, input.tex0.xy / input.tex0.w);
	else if (U[5].y != 0.0)
		texel0 = Texture3.Sample(Sampler3, input.tex0.xyz / input.tex0.w);
	if (U[5].x != 0.0 || U[5].y != 0.0)
		color = CombineUnit(color, primary, texel0, U[1], U[5], U[7], U[9], U[10], U[11]);
	if (U[6].x != 0.0) {
		float4 texel1 = Texture1.Sample(Sampler1, input.tex1.xy / input.tex1.w);
		color = CombineUnit(color, primary, texel1, U[2], U[6], U[8], U[12], U[13], U[14]);
	}
	if (U[4].w != 0.0)
		color.rgb = saturate(color.rgb + input.secondary.rgb);
	if (U[15].x != 0.0 && !AlphaPass(color.a, (int)U[4].y, U[3].x))
		discard;
	if (U[15].y != 0.0) {
		float fogFactor;
		int fogMode = (int)U[4].x;
		float fogValue = input.fogCoord;
		if (fogMode == 9729)
			fogFactor = (U[3].w - fogValue) / max(U[3].w - U[3].z, 0.00001);
		else if (fogMode == 2049) {
			float d = U[3].y * fogValue;
			fogFactor = exp(-(d * d));
		} else
			fogFactor = exp(-(U[3].y * fogValue));
		color.rgb = lerp(U[0].rgb, color.rgb, saturate(fogFactor));
	}
	return saturate(color);
}
