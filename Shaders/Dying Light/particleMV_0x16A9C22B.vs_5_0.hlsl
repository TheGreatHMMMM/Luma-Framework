// ---- Created with 3Dmigoto v1.3.16 on Sat Apr 11 05:44:24 2026
#include "Includes/Common.hlsl"
cbuffer cb2 : register(b2)
{
  float4 cb2[34];
}

cbuffer cb0 : register(b0)
{
  float4 cb0[8];
}




// 3Dmigoto declarations
#define cmp -


void main(
  float4 v0 : POSITION0,
  out float4 o0 : SV_Position0,
  out float4 o1 : TEXCOORD0,
  out float4 o2 : TEXCOORD1)
{
  float4 r0,r1,r2,r3;
  uint4 bitmask, uiDest;
  float4 fDest;

  r0.x = 4096 * v0.z;
  r0.x = floor(r0.x);
  r0.xyz = r0.xxx * float3(60.0443344,69.9532166,69.8011093) + float3(73.1643372,64.3676224,89.1583557);
  r0.xyz = frac(r0.xyz);
  r0.xyz = r0.xyz * float3(2,2,2) + float3(-1,-1,-1);
  r0.w = dot(r0.xyz, float3(3.1400001,3.1400001,3.1400001));
  r1.xyz = r0.www * float3(23.8791599,13.7204599,46.5288811) + cb0[6].xxx;
  r2.xyz = r0.www * float3(23.8791599,13.7204599,46.5288811) + cb0[7].xxx;
  r2.xyz = sin(r2.xyz);
  r2.xyz = r2.xyz * cb0[2].www + cb0[3].xyz;
  r2.xyz = r2.xyz + r0.xyz;
  r2.xyz = -cb0[0].xyz + r2.xyz;
  r2.xyz = frac(r2.xyz);
  r2.xyz = r2.xyz * float3(2,2,2) + -cb0[1].xyz;
  r2.xyz = float3(-1,-1,-1) + r2.xyz;
  r2.xyz = cb0[0].www * r2.xyz;
  r1.xyz = sin(r1.xyz);
  r1.xyz = r1.xyz * cb0[2].www + cb0[2].xyz;
  r0.xyz = r1.xyz + r0.xyz;
  r0.xyz = -cb0[0].xyz + r0.xyz;
  r0.xyz = frac(r0.xyz);
  r0.xyz = r0.xyz * float3(2,2,2) + float3(-1,-1,-1);
  r0.xyz = -cb0[1].xyz + r0.xyz;
  r0.w = dot(r0.xyz, r0.xyz);
  r0.xyz = cb0[0].www * r0.xyz;
  r0.w = saturate(cb0[1].w * r0.w);
  r0.w = sqrt(r0.w);
  r1.xyz = v0.xyz * float3(0.5,0.5,0.5) + float3(0.5,0.5,0.5);
  r1.z = saturate(r1.z * cb0[4].x + cb0[4].y);
  r1.xy = r1.xy * cb0[4].zz + -cb0[4].ww;
  r0.w = r1.z * r0.w;
  r3.x = dot(r0.xyz, cb2[4].xyz);
  r3.y = dot(r0.xyz, cb2[5].xyz);
  r3.z = dot(r0.xyz, cb2[6].xyz);
  r0.xy = r1.xy * r0.ww + r3.xy;
  r3.w = 1;
  r0.zw = r3.zw;
  o0.x = dot(r0.xyzw, cb2[30].xyzw);
  o0.y = dot(r0.xyzw, cb2[31].xyzw);
  o0.z = dot(r0.xyzw, cb2[32].xyzw);
  o0.w = dot(r0.xyzw, cb2[33].xyzw);
  o1.xyzw = 0;
  r0.x = dot(r3.xyzw, cb2[30].xyzw);
  r0.y = dot(r3.xyzw, cb2[31].xyzw);
  r0.z = dot(r3.xyzw, cb2[33].xyzw);
  r0.xy = r0.xy / r0.zz;
  r1.x = dot(r2.xyz, cb2[4].xyz);
  r1.y = dot(r2.xyz, cb2[5].xyz);
  r1.z = dot(r2.xyz, cb2[6].xyz);
  r1.w = 1;
  r2.x = dot(r1.xyzw, cb2[30].xyzw);
  r2.y = dot(r1.xyzw, cb2[31].xyzw);
  r0.z = dot(r1.xyzw, cb2[33].xyzw);
  r0.zw = r2.xy / r0.zz;
  r0.xy = r0.xy + -r0.zw;
  o2.xyzw = 0;
  o1.xyzw = 0;
  return;
}