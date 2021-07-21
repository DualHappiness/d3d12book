//***************************************************************************************
// Default.hlsl by Frank Luna (C) 2015 All Rights Reserved.
//
// Default shader, currently supports lighting.
//***************************************************************************************

// Defaults for number of lights.
#ifndef NUM_DIR_LIGHTS
    #define NUM_DIR_LIGHTS 3
#endif

#ifndef NUM_POINT_LIGHTS
    #define NUM_POINT_LIGHTS 0
#endif

#ifndef NUM_SPOT_LIGHTS
    #define NUM_SPOT_LIGHTS 0
#endif

// Include structures and functions for lighting.
#include "LightingUtil.hlsl"

Texture2D    gDiffuseMap : register(t0);


SamplerState gsamPointWrap        : register(s0);
SamplerState gsamPointClamp       : register(s1);
SamplerState gsamLinearWrap       : register(s2);
SamplerState gsamLinearClamp      : register(s3);
SamplerState gsamAnisotropicWrap  : register(s4);
SamplerState gsamAnisotropicClamp : register(s5);

// Constant data that varies per frame.
cbuffer cbPerObject : register(b0)
{
    float4x4 gWorld;
	float4x4 gTexTransform;
};

// Constant data that varies per material.
cbuffer cbPass : register(b1)
{
    float4x4 gView;
    float4x4 gInvView;
    float4x4 gProj;
    float4x4 gInvProj;
    float4x4 gViewProj;
    float4x4 gInvViewProj;
    float3 gEyePosW;
    float cbPerObjectPad1;
    float2 gRenderTargetSize;
    float2 gInvRenderTargetSize;
    float gNearZ;
    float gFarZ;
    float gTotalTime;
    float gDeltaTime;
    float4 gAmbientLight;

	float4 gFogColor;
	float gFogStart;
	float gFogRange;
	float2 cbPerObjectPad2;

    // Indices [0, NUM_DIR_LIGHTS) are directional lights;
    // indices [NUM_DIR_LIGHTS, NUM_DIR_LIGHTS+NUM_POINT_LIGHTS) are point lights;
    // indices [NUM_DIR_LIGHTS+NUM_POINT_LIGHTS, NUM_DIR_LIGHTS+NUM_POINT_LIGHT+NUM_SPOT_LIGHTS)
    // are spot lights for a maximum of MaxLights per object.
    Light gLights[MaxLights];
};

cbuffer cbMaterial : register(b2)
{
	float4   gDiffuseAlbedo;
    float3   gFresnelR0;
    float    gRoughness;
	float4x4 gMatTransform;
};

struct VertexIn
{
	float3 PosL    : POSITION;
    float3 NormalL : NORMAL;
	float2 TexC    : TEXCOORD;
};

struct VertexOut
{
    float3 PosL    : POSITION;
    float3 NormalL : NORMAL;
	float2 Tex    : TEXCOORD;
};

struct GeoOut
{
    float4 PosH : SV_POSITION;
    float3 PosW : POSITION;
    float3 NormalW : NORMAL;
    float2 Tex : TEXCOORD;
};

VertexOut VS(VertexOut vin)
{
    return vin;
}

GeoOut GeoVS(VertexOut vin)
{
    GeoOut vout;
        // Transform to world space.
    float4 posW = mul(float4(vin.PosL, 1.0f), gWorld);
    vout.PosW = posW.xyz;

    // Assumes nonuniform scaling; otherwise, need to use inverse-transpose of world matrix.
    vout.NormalW = mul(vin.NormalL, (float3x3)gWorld);

    // Transform to homogeneous clip space.
    vout.PosH = mul(posW, gViewProj);
	
	// Output vertex attributes for interpolation across triangle.
	float4 texC = mul(float4(vin.Tex, 0.0f, 1.0f), gTexTransform);
	vout.Tex = mul(texC, gMatTransform).xy;

    return vout;
}

float4 PS(GeoOut pin) : SV_Target
{
    float4 diffuseAlbedo = gDiffuseMap.Sample(gsamAnisotropicWrap, pin.Tex) * gDiffuseAlbedo;
	
#ifdef ALPHA_TEST
	// Discard pixel if texture alpha < 0.1.  We do this test as soon 
	// as possible in the shader so that we can potentially exit the
	// shader early, thereby skipping the rest of the shader code.
	clip(diffuseAlbedo.a - 0.1f);
#endif

    // Interpolating normal can unnormalize it, so renormalize it.
    pin.NormalW = normalize(pin.NormalW);

    // Vector from point being lit to eye. 
	float3 toEyeW = gEyePosW - pin.PosW;
	float distToEye = length(toEyeW);
	toEyeW /= distToEye; // normalize

    // Light terms.
    float4 ambient = gAmbientLight*diffuseAlbedo;

    const float shininess = 1.0f - gRoughness;
    Material mat = { diffuseAlbedo, gFresnelR0, shininess };
    float3 shadowFactor = 1.0f;
    float4 directLight = ComputeLighting(gLights, mat, pin.PosW,
        pin.NormalW, toEyeW, shadowFactor);

    float4 litColor = ambient + directLight;

#ifdef FOG
	float fogAmount = saturate((distToEye - gFogStart) / gFogRange);
	litColor = lerp(litColor, gFogColor, fogAmount);
#endif

    // Common convention to take alpha from diffuse albedo.
    litColor.a = diffuseAlbedo.a;

    return litColor;
}

void Subdivide(VertexOut inVerts[3], out VertexOut outVerts[6]){
    VertexOut mid[3];
    mid[0].PosL = 0.5 * (inVerts[0].PosL + inVerts[1].PosL);
    mid[1].PosL = 0.5 * (inVerts[1].PosL + inVerts[2].PosL);
    mid[2].PosL = 0.5 * (inVerts[2].PosL + inVerts[0].PosL);

    // 落回球体
    mid[0].PosL = normalize(mid[0].PosL);
    mid[1].PosL = normalize(mid[1].PosL);
    mid[2].PosL = normalize(mid[2].PosL);

    mid[0].NormalL = mid[0].PosL;
    mid[1].NormalL = mid[1].PosL;
    mid[2].NormalL = mid[2].PosL;

    mid[0].Tex = 0.5 * (inVerts[0].Tex + inVerts[1].Tex);
    mid[1].Tex = 0.5 * (inVerts[1].Tex + inVerts[2].Tex);
    mid[2].Tex = 0.5 * (inVerts[2].Tex + inVerts[0].Tex);

    // * 这里已经不关心三角形的缠绕顺序了, 剔除再之前以及做了
    outVerts[0] = inVerts[0];
    outVerts[1] = mid[0];
    outVerts[2] = mid[2];
    outVerts[3] = mid[1];
    outVerts[4] = inVerts[2];
    outVerts[5] = inVerts[1];
}

void TransVertexToGeo(VertexOut v[6], int size, out GeoOut gout[6])
{
    [unroll]
    for(int i = 0; i < size; ++i)
    {
        float4 PosW = mul(float4(v[i].PosL, 1.0f), gWorld);
        gout[i].PosW = PosW.xyz;
        gout[i].NormalW = mul(v[i].NormalL, (float3x3)gWorld); // 没有非等比缩放
        gout[i].PosH = mul(PosW, gViewProj);
        gout[i].Tex = v[i].Tex;
    }
}

void OutputVertex(GeoOut gout[6], inout TriangleStream<GeoOut> triStream)
{
    [unroll]
    for(int i = 0; i < 5; ++i)
    {
        triStream.Append(gout[i]);
    }
    triStream.RestartStrip();
    triStream.Append(gout[1]);
    triStream.Append(gout[5]);
    triStream.Append(gout[3]);
}

[maxvertexcount(32)]
void GS(triangle VertexOut gin[3], 
        inout TriangleStream<GeoOut> triStream)
{	
    float distance = length(gEyePosW);

    float3 l1 = gin[1].PosL - gin[0].PosL;
    float3 l2 = gin[2].PosL - gin[0].PosL;
    float3 pn = normalize(cross(l1, l2));

    gin[0].PosL += gTotalTime * pn;
    gin[1].PosL += gTotalTime * pn;
    gin[2].PosL += gTotalTime * pn;

    if(distance > 10.0)
    {
        VertexOut v[6];
        v[0] = gin[0];
        v[1] = gin[1];
        v[2] = gin[2];

        GeoOut gout[6];
        TransVertexToGeo(v, 3, gout);
        triStream.Append(gout[0]);
        triStream.Append(gout[1]);
        triStream.Append(gout[2]);
    }
    else if (distance > 5.0)
    {
        VertexOut v[6];
        Subdivide(gin, v);
        GeoOut gout[6];
        TransVertexToGeo(v, 6, gout);
        OutputVertex(gout, triStream);
    }
    else
    {
        // 不追求好看了 要不然应该定义一个函数划分新三角形列表 
        VertexOut v[6];
        Subdivide(gin, v);

        VertexOut subsubv[6];
        VertexOut subv[3];
        GeoOut gout[6];

        subv[0] = v[0];
        subv[1] = v[1];
        subv[2] = v[2];
        Subdivide(subv, subsubv);
        TransVertexToGeo(subsubv, 6, gout);
        OutputVertex(gout, triStream);
        triStream.RestartStrip();

        subv[0] = v[2];
        subv[1] = v[1];
        subv[2] = v[3];
        Subdivide(subv, subsubv);
        TransVertexToGeo(subsubv, 6, gout);
        OutputVertex(gout, triStream);
        triStream.RestartStrip();
        
        subv[0] = v[2];
        subv[1] = v[3];
        subv[2] = v[4];
        Subdivide(subv, subsubv);
        TransVertexToGeo(subsubv, 6, gout);
        OutputVertex(gout, triStream);
        triStream.RestartStrip();

        subv[0] = v[1];
        subv[1] = v[5];
        subv[2] = v[3];
        Subdivide(subv, subsubv);
        TransVertexToGeo(subsubv, 6, gout);
        OutputVertex(gout, triStream);
        triStream.RestartStrip();
    }
}