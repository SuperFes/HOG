import tone;

[[vk::binding(0, 0)]] Sampler2D<float> depth_image;

struct Push {
    float4x4 eye_from_clip;
    // xyz towards the light, w the exposure.
    float4 towards_exposure;
    // rgb reaching the eye.
    float4 irradiance;
    // xy the light's pixel, z its disc's radius in pixels, w 1 where it's in front of the eye.
    float4 disc;
};

[[vk::push_constant]] ConstantBuffer<Push> push;

static const float degrees_per_radian = 57.2957795;
// Nothing drawn is behind the sky, which sky.slang draws at 1e-7.
static const float sky_depth = 2e-7;
static const uint ring_points = 16;
static const float ring_share = 0.7; // of the disc's radius

struct Glow {
    float4 position : SV_Position;
    float2 clip;
};

// As glare.hpp's eye_spread.
float eye_spread(float degrees) {
    const float near = degrees + 0.02;
    return ((0.478 * 20.91) / (near * near * near)) + ((0.138 * 72.37) / (near * near));
}

bool seen_at(float2 pixel, uint2 size) {
    if (any(pixel < 0.0) || any(pixel >= float2(size))) {
        return true; // what's out of the picture can't be in front
    }
    return depth_image.Load(int3(int2(pixel), 0)) <= sky_depth;
}

// The share of the light's disc not hidden by what's drawn.
float seen() {
    if (push.disc.w == 0.0) {
        return 1.0;
    }
    uint2 size;
    depth_image.GetDimensions(size.x, size.y);
    float seen = seen_at(push.disc.xy, size) ? 1.0 : 0.0;
    for (uint at = 0; at < ring_points; ++at) {
        const float turn = 6.2831853 * (float(at) / float(ring_points));
        const float2 offset = float2(cos(turn), sin(turn)) * (push.disc.z * ring_share);
        seen += seen_at(push.disc.xy + offset, size) ? 1.0 : 0.0;
    }
    return seen / float(ring_points + 1);
}

// A triangle covering the whole picture.
[shader("vertex")]
Glow vertex_main(uint id: SV_VertexID) {
    const float2 clip = (float2((id << 1) & 2, id & 2) * 2.0) - 1.0;
    Glow glow;
    glow.position = float4(clip, 0.0, 1.0);
    glow.clip = clip;
    return glow;
}

[shader("fragment")]
float4 fragment_main(Glow glow) : SV_Target {
    const float4 far = mul(push.eye_from_clip, float4(glow.clip, 1.0, 1.0));
    const float3 view = normalize(far.xyz / far.w);
    // Exact close to the light, where acos of the dot isn't.
    const float angle = 2.0 * asin(min(length(view - push.towards_exposure.xyz) * 0.5, 1.0));
    const float3 glare =
        push.irradiance.rgb * (seen() * eye_spread(angle * degrees_per_radian));
    return float4(tone_mapped(glare, push.towards_exposure.w), 0.0);
}
