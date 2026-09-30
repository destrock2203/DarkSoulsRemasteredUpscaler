// Full-screen copy with format conversion / scaling: a triangle covering the viewport samples the
// source across its whole extent (bilinear when scaling, exact when the sizes match).

Texture2D<float4> g_source : register(t0);
SamplerState g_sampler : register(s0);

struct VsOut {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VsOut vs_main(uint id : SV_VertexID) {
    VsOut o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.position = float4(o.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

float4 ps_main(VsOut i) : SV_Target {
    return g_source.SampleLevel(g_sampler, i.uv, 0);
}
