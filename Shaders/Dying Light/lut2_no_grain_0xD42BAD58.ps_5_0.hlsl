Texture2D<float4> t2 : register(t2);
Texture2D<float4> t1 : register(t1);
Texture2D<float4> t0 : register(t0);

SamplerState s2_s : register(s2);
SamplerState s1_s : register(s1);
SamplerState s0_s : register(s0);

cbuffer cb0 : register(b0)
{
  float4 cb0[3];
}

float Neutwo(float x) {
  // also written as x * rhypot(x, 1.0)
  float numerator = x;
  float denominator_squared = mad(x, x, 1.0);
  return numerator * rsqrt(denominator_squared);
}

float ComputeMaxChannelScale(float3 color) {
    float max_channel = max(color.r, max(color.g, color.b));
  float new_max = Neutwo(max_channel);
  float scale = max_channel != 0 ? (new_max / max_channel) : 1.f;
  return scale;
}

void main(
    float4 v0 : SV_POSITION0,
    float4 v1 : TEXCOORD0,
    float4 v2 : TEXCOORD1,
    out float4 o0 : SV_TARGET0)
{
  float4 r0, r1;

  // not sure what this code does, no observed effect when i skip it
  r0.xy = -abs(v2.xy) * abs(v2.xy) + float2(1, 1);
  r0.x = saturate(-r0.x * r0.y + 1); // r0.x = saturate(-r0.x * r0.y + 1);
  r0.x = cb0[2].x * r0.x;
  r0.x = cb0[0].w * r0.x;
  r0.y = t0.SampleLevel(s0_s, v1.xy, 0).x;
  r0.z = t0.SampleLevel(s0_s, v1.zw, 0).z;
  r1.xyzw = t0.SampleLevel(s0_s, v2.zw, 0).xyzw; // render
  r0.yz = -r1.xz + r0.yz;
  r1.xz = r0.xx * r0.yz + r1.xz;
  o0.w = r1.w;
  r0.x = dot(float3(0.212500006, 0.715399981, 0.0720999986), r1.xyz); // sRGB Luminance
  r0.yzw = r0.x + -r1.xyz;
  r0.x = saturate(r0.x * cb0[0].y + cb0[0].z);  // r0.x = saturate(r0.x * cb0[0].y + cb0[0].z);
  r0.x = cb0[0].x * r0.x;
  r0.xyz = r0.x * r0.yzw + r1.xyz;
  r0.w = dot(r0.xyz, cb0[1].xyz);
  r0.xyz = r0.xyz * cb0[1].w + r0.w; // r0.xyz = saturate(r0.xyz * cb0[1].w + r0.w);

  float scale = ComputeMaxChannelScale(r0.xyz);
  r0.xyz = r0.xyz * scale;

  // LUT
  r0.xyz = sign(r0.xyz) * pow(abs(r0.xyz), 1.f / 2.2f); // TODO: copy LUT extrapolation from Hollow Knight 2, or copy improved LUT sampling from ... (ask Pumbo). Also copy LUT smoothing from Hollow Knight 2.
  float lutSize = 256.0;
  r0.xyz = (r0.xyz * ((lutSize - 1.0) / lutSize)) + (0.5 / lutSize); // Remap for LUT input (acknowledge half texel offset)
  r1.x = t2.Sample(s2_s, r0.x).x;
  r1.y = t2.Sample(s2_s, r0.y).y;
  r1.z = t2.Sample(s2_s, r0.z).z;
  r0.xyz = sign(r1.xyz) * pow(abs(r1.xyz), 2.2f);

  r0.xyz = r0.xyz / scale;

  r1.xyz = t1.SampleLevel(s1_s, v2.zw, 0).xyz;
  o0.xyz = r1.xyz * r0.xyz; // o0.xyz = saturate(r1.xyz * r0.xyz);
}