// Full-screen motion vectors and depth for the upscalers / frame generation, rebuilt exactly as the
// game's TAA resolve (FRPG_Fil_ResolveTAA.fpo) derives them, but per pixel (no 2x2 closest-depth
// dilation: DLSS / FSR / XeSS dilate on their own):
//  - object motion where the velocity pass (FRPG_*_Vel shaders) wrote one: (curNDC - prevNDC) / 2,
//    y up; pixels without object motion keep the clear value (x >= 1)
//  - elsewhere camera motion: the depth reprojected with the resolve's own matrices, clip -> world
//    (cb0[58..61]), plus the camera offset between frames (cb0[78], the engine renders camera
//    relative), then world -> previous clip (cb0[74..77])
// Output: prevUV - curUV in render-extent UV units (y down), the convention of Streamline's mvecScale
// = 1, and of FSR / XeSS once scaled by the render size. Depth is copied as it is (0 near, 1 far).

Texture2D<float> g_depth : register(t0);
Texture2D<float2> g_velocity : register(t1);
SamplerState g_point : register(s0);
RWTexture2D<float2> g_motion : register(u0);
RWTexture2D<float> g_depthOut : register(u1);

cbuffer Constants : register(b0) {
    float4 g_size;              // render width, height, 1 / width, 1 / height
    float4 g_clipToWorld[4];    // resolve cb0[58..61], rows
    float4 g_worldToPrevClip[4];// resolve cb0[74..77], rows
    float4 g_cameraOffset;      // resolve cb0[78].xyz
    uint4 g_flags;              // x: a velocity texture is bound
};

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= (uint)g_size.x || id.y >= (uint)g_size.y) return;
    const float2 uv = (float2(id.xy) + 0.5) * g_size.zw;
    const float depth = g_depth.SampleLevel(g_point, uv, 0);

    float2 motion;  // curUV - prevUV, as the resolve computes it
    const float2 velocity = g_flags.x ? g_velocity.SampleLevel(g_point, uv, 0) : float2(2.0, 2.0);
    if (velocity.x < 1.0) {
        motion = float2(velocity.x, -velocity.y);
    } else {
        const float4 clip = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), depth, 1.0);
        const float4 world = float4(dot(clip, g_clipToWorld[0]), dot(clip, g_clipToWorld[1]),
                                    dot(clip, g_clipToWorld[2]), dot(clip, g_clipToWorld[3]));
        const float4 position = float4(world.xyz / world.w + g_cameraOffset.xyz, 1.0);
        const float3 prev = float3(dot(position, g_worldToPrevClip[0]), dot(position, g_worldToPrevClip[1]),
                                   dot(position, g_worldToPrevClip[3]));
        const float2 prevUv = prev.xy / prev.z * float2(0.5, -0.5) + 0.5;
        motion = uv - prevUv;
    }
    g_motion[id.xy] = -motion;
    g_depthOut[id.xy] = depth;
}
