import atmosphere;
import shadow;
import tone;
import water;

[[vk::binding(0, 0)]] Sampler2D<float> depth_image;
[[vk::binding(1, 0)]] ConstantBuffer<Lighting> lighting;
// As cloud_march.slang's.
[[vk::binding(2, 0)]] Sampler2D cloud_shadow;
// As cloud_resolve.slang leaves them, premultiplied: a the share of the view they hide.
[[vk::binding(3, 0)]] Sampler2D clouds;

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
// Of the picture's height, how far in from its edge the light's disc is wholly seen through the
// clouds marched for it, rather than the cloud's shadow over the eye.
static const float clouds_edge = 0.05;

struct Glow {
    float4 position : SV_Position;
    float2 clip;
    nointerpolation float seen;
};

float2 ring_point(uint at) {
    const float turn = 6.2831853 * (float(at) / float(ring_points));
    return float2(cos(turn), sin(turn)) * (push.disc.z * ring_share);
}

// As glare.hpp's eye_spread, eye_spread_streaked and streaks.
float eye_spread(float degrees) {
    const float near = degrees + 0.02;
    return ((0.478 * 20.91) / (near * near * near)) + ((0.138 * 72.37) / (near * near));
}

float eye_spread_streaked(float degrees) {
    const float near = degrees + 0.02;
    return (0.478 * 20.91) / (near * near * near) * exp(-degrees / 10.0);
}

static const uint streak_count = 96;
static const float streak_width = 0.12;
static const float streaked = 0.8;

float stride(uint streak, uint step) {
    return (float((streak * step) % streak_count) + 0.5) / float(streak_count);
}

float streaks(float turn) {
    const float at = frac(turn) * float(streak_count);
    const uint streak = uint(at) % streak_count;
    const float brightness = 5.0 * pow(stride(streak, 59), 4.0);
    const float across = (frac(at) - (0.5 + (0.6 * (stride(streak, 37) - 0.5)))) / streak_width;
    return (1.0 - streaked) +
           (streaked * brightness * exp(-across * across) / (streak_width * sqrt(3.14159265)));
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
        seen += seen_at(push.disc.xy + ring_point(at), size) ? 1.0 : 0.0;
    }
    return seen / float(ring_points + 1);
}

// Of the light, the share any cloud, fog and mist let straight through to the eye: as the clouds
// marched for the picture have it over the light's disc, as they're seen, with the fog among them,
// through the haze; else, out of the picture, as the cloud's shadow and the mist over the eye have
// it.
float through_clouds() {
    const float shadowed = shade_at(lighting, cloud_shadow, float3(0.0)).sun;
    if (push.disc.w == 0.0) {
        return shadowed;
    }
    uint2 size;
    depth_image.GetDimensions(size.x, size.y);
    const float2 picture = float2(size);
    const float2 from_edge = min(push.disc.xy, picture - push.disc.xy);
    const float within = saturate(min(from_edge.x, from_edge.y) / (clouds_edge * picture.y));
    if (within <= 0.0) {
        return shadowed;
    }
    float hidden = clouds.SampleLevel(push.disc.xy / picture, 0.0).a;
    for (uint at = 0; at < ring_points; ++at) {
        hidden += clouds.SampleLevel((push.disc.xy + ring_point(at)) / picture, 0.0).a;
    }
    const float marched = saturate(1.0 - (hidden / float(ring_points + 1))) * lighting.shadow.w;
    return lerp(shadowed, marched, within);
}

// A triangle covering the whole picture, with how much of the light is seen worked out once for
// it rather than for every pixel.
[shader("vertex")]
Glow vertex_main(uint id: SV_VertexID) {
    const float2 clip = (float2((id << 1) & 2, id & 2) * 2.0) - 1.0;
    Glow glow;
    glow.position = float4(clip, 0.0, 1.0);
    glow.clip = clip;
    glow.seen = seen() * through_clouds();
    return glow;
}

[shader("fragment")]
float4 fragment_main(Glow glow) : SV_Target {
    // From in the water, the sea's pass has laid it over all that's seen.
    if (seen_from_water(lighting, glow.clip)) {
        discard;
    }
    const float4 far = mul(push.eye_from_clip, float4(glow.clip, 1.0, 1.0));
    const float3 view = normalize(far.xyz / far.w);
    // Exact close to the light, where acos of the dot isn't.
    const float angle = 2.0 * asin(min(length(view - push.towards_exposure.xyz) * 0.5, 1.0));
    const float degrees = angle * degrees_per_radian;
    float spread = eye_spread(degrees);
    if (push.disc.w != 0.0) {
        // Streaks are in the eye, so they're laid round the light as it is in the picture.
        const float2 away = glow.position.xy - push.disc.xy;
        const float turn = atan2(away.y, away.x) / 6.2831853;
        spread += eye_spread_streaked(degrees) * (streaks(turn) - 1.0);
    }
    const float3 glare = push.irradiance.rgb * (glow.seen * spread);
    return float4(tone_mapped(glare, push.towards_exposure.w), 0.0);
}
