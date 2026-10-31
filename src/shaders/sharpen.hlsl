// Sharpening filters on the display-referred image (0 - 1), RGBA16F in and out (ported from
// EldenRingUpscaler's render/sharpen):
//
// Mode 0: AMD FidelityFX RCAS (robust contrast-adaptive sharpening, the sharpening pass of FSR 1:
// ffx_fsr1.h, MIT license, Copyright (c) 2021 Advanced Micro Devices, Inc.), with its noise removal
// (FSR_RCAS_DENOISE): the 4-neighbour cross weighted by the largest negative lobe that keeps the result
// within the local min / max, so it can't ring or clip. Strength = FsrRcasCon's value, exp2(-stops).
//
// Mode 1: Laplacian: the 4-neighbour Laplacian in log space, clamped, added back (the filter Elden
// Ring's TAA sharpening pass uses):
//     L(x) = log(x + 1e-4)
//     lap  = clamp(4 L(center) - L(up) - L(down) - L(left) - L(right), RangeMin, RangeMax)
//     out  = max(0, exp(L(center) + Strength * lap) - 1e-4)
// Crisper than RCAS; the clamp bounds the halos.

Texture2D<float4> g_src : register(t0);
RWTexture2D<float4> g_dst : register(u0);

cbuffer Constants : register(b0) {
    uint2 g_size;
    float g_strength;
    uint g_mode;
    float g_rangeMin;
    float g_rangeMax;
};

float4 Fetch(int2 p) { return g_src.Load(int3(clamp(p, int2(0, 0), int2(g_size) - 1), 0)); }

float Max3(float3 v) { return max(v.r, max(v.g, v.b)); }
float Luma2(float3 c) { return c.b * 0.5 + (c.r * 0.5 + c.g); }

//    b
//  d e f
//    h
float3 Rcas(int2 p, float3 e) {
    e = saturate(e);
    const float3 b = saturate(Fetch(p + int2(0, -1)).rgb);
    const float3 d = saturate(Fetch(p + int2(-1, 0)).rgb);
    const float3 f = saturate(Fetch(p + int2(1, 0)).rgb);
    const float3 h = saturate(Fetch(p + int2(0, 1)).rgb);
    const float kLimit = 0.25 - 1.0 / 16.0;  // FSR_RCAS_LIMIT
    const float3 mn4 = min(min(b, d), min(f, h));
    const float3 mx4 = max(max(b, d), max(f, h));
    // Lobe limits that keep the output within [0, 1] and the neighbourhood (guarded where the
    // original's approximate reciprocals would divide by zero).
    const float3 hitMin = min(mn4, e) / max(4.0 * mx4, 1e-5);
    const float3 hitMax = (1.0 - max(mx4, e)) / min(4.0 * mn4 - 4.0, -1e-5);
    const float3 lobeRGB = max(-hitMin, hitMax);
    float lobe = max(-kLimit, min(Max3(lobeRGB), 0.0)) * g_strength;
    // Noise removal: less sharpening where the centre alone stands out.
    const float bL = Luma2(b), dL = Luma2(d), eL = Luma2(e), fL = Luma2(f), hL = Luma2(h);
    const float range = max(max(max(bL, dL), max(eL, fL)), hL) - min(min(min(bL, dL), min(eL, fL)), hL);
    const float nz = saturate(abs(0.25 * (bL + dL + fL + hL) - eL) / max(range, 1e-5));
    lobe *= -0.5 * nz + 1.0;
    return (lobe * (b + d + f + h) + e) / (4.0 * lobe + 1.0);
}

float3 L(int2 p) { return log(max(Fetch(p).rgb, 0.0) + 1e-4); }

float3 Laplacian(int2 p, float3 center) {
    const float3 c = log(max(center, 0.0) + 1e-4);
    float3 lap = 4.0 * c - L(p + int2(0, -1)) - L(p + int2(0, 1)) - L(p + int2(-1, 0)) - L(p + int2(1, 0));
    lap = clamp(lap, g_rangeMin, g_rangeMax);
    return saturate(exp(c + g_strength * lap) - 1e-4);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= g_size)) return;
    const int2 p = int2(id.xy);
    const float4 center = Fetch(p);
    const float3 rgb = g_mode == 1 ? Laplacian(p, center.rgb) : Rcas(p, center.rgb);
    g_dst[id.xy] = float4(rgb, center.a);
}
