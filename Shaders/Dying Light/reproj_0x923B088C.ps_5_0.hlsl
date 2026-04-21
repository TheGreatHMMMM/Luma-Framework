// ---- Created with 3Dmigoto v1.3.16 on Sat Apr 11 05:45:04 2026
#include "Includes/Common.hlsl"
Texture2D<float4> t0 : register(t0);

SamplerState s0_s : register(s0);

cbuffer cb2 : register(b2)
{
  float4 cb2[13];
}

cbuffer cb0 : register(b0)
{
  float4 cb0[5];
}




// 3Dmigoto declarations
#define cmp -


void main(
  float4 v0 : SV_POSITION0,
  float2 v1 : TEXCOORD0,
  out float4 o0 : SV_TARGET0)
{
  float4 r0,r1,r2;
  uint4 bitmask, uiDest;
  float4 fDest;

  r0.x = t0.SampleLevel(s0_s, v1.xy, 0).x;
  r0.xy = r0.xx * cb2[8].xy + cb2[8].zw;
  r0.x = r0.x / -r0.y;
  r0.yz = (v0.xy ) * cb2[9].xy + cb2[9].zw;
  r1.xy = r0.yz * abs(r0.xx);
  r1.z = -abs(r0.x);
  r1.w = 1;
  r0.x = dot(r1.xyzw, cb2[10].xyzw);
  r0.y = dot(r1.xyzw, cb2[11].xyzw);
  r0.z = dot(r1.xyzw, cb2[12].xyzw);
  r0.w = 1;
  r1.x = dot(r0.xyzw, cb2[0].xyzw);
  r1.y = dot(r0.xyzw, cb2[1].xyzw);
  r1.z = dot(r0.xyzw, cb2[3].xyzw);
  r1.xy = r1.xy / r1.zz;
  r2.x = dot(r0.xyzw, cb0[1].xyzw);
  r2.y = dot(r0.xyzw, cb0[2].xyzw);
  r0.x = dot(r0.xyzw, cb0[4].xyzw);
  r0.xy = r2.xy / r0.xx;
  r0.xy = r1.xy + -r0.xy;
  o0.xy = r0.xy * cb0[0].zw; // cb0[0].xy;
  o0.zw = float2(0,0);
  return;
}