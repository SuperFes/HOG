import atmosphere;
import shadow;
import tone;
import weather;

[[vk::binding(0, 0)]] ConstantBuffer<Air> air;
[[vk::binding(1, 0)]] ConstantBuffer<Lighting> lighting;
[[vk::binding(2, 0)]] Sampler2D transmittance_table;
[[vk::binding(3, 0)]] Sampler2DArray aerial_volume;
[[vk::binding(4, 0)]] Sampler2D<float> depth_image;
// As client::SkyTexel.
[[vk::binding(5, 0)]] SamplerCube sky_before;
[[vk::binding(6, 0)]] SamplerCube sky_after;
// As cloud_march.slang leaves them.
[[vk::binding(7, 0)]] Sampler2D marched;
[[vk::binding(8, 0)]] Sampler2D<float> distances;
// As cloud_march.slang's.
[[vk::binding(9, 0)]] Sampler2D cloud_shadow;

struct Push {
    float4x4 eye_from_clip;
    // xyz up through the eye, w its height above sea level.
    float4 up_height;
    // x the planet's radius, y the faked radius drawing bends it onto, z how far the weather has
    // faded from the maps before to those after, w the highest fog's top anywhere, in metres above
    // sea level, 0 for none.
    float4 radii;
};

[[vk::push_constant]] ConstantBuffer<Push> push;

// Nothing drawn is behind the sky, which sky.slang draws at 1e-7.
static const float sky_depth = 2e-7;
// Metres along the view fog's marched over from over it, at most.
static const float far_fog_reach = 40000.0;
static const uint fog_steps = 24;
// Metres of fog marched through at most from within it, past which it hides everything.
static const float fog_reach = 3000.0;

struct Layer {
    float4 position : SV_Position;
    float2 clip;
};

// A triangle covering the whole picture.
[shader("vertex")]
Layer vertex_main(uint id: SV_VertexID) {
    const float2 clip = (float2((id << 1) & 2, id & 2) * 2.0) - 1.0;
    Layer layer;
    layer.position = float4(clip, 0.0, 1.0);
    layer.clip = clip;
    return layer;
}

SkyCell sky_at(float3 direction) {
    return sky_cell(lerp(sky_before.SampleLevel(direction, 0.0),
                         sky_after.SampleLevel(direction, 0.0), push.radii.z));
}

// The fog from `start` to `end` along the view, premultiplied, before tone mapping. Each step takes
// all the fog in it, however thin, from the weather at a point in it offset by `jitter`; steps are
// finest at the start, where the fog's nearest.
float4 fog_between(float3 view, float start, float end, float2 uv, float jitter) {
    const float3 up = push.up_height.xyz;
    const float eye_height = push.up_height.w;
    const float radius = push.radii.x;
    const float faked = push.radii.y;
    const float3 sun = lighting.sun_exposure.xyz;
    const float scale = lighting.sky_scale.w;
    const float cos_sun = dot(view, sun);

    const float length = end - start;
    float3 radiance = float3(0.0);
    float seen_through = 1.0;
    float distance = 0.0;
    float from = start;
    float from_height = drawn_height(view * from, up, eye_height, faked);
    for (uint at = 0; at < fog_steps; ++at) {
        const float share = float(at + 1) / float(fog_steps);
        const float to = start + (length * share * share);
        const float to_height = drawn_height(view * to, up, eye_height, faked);
        const float along = lerp(from, to, jitter);
        const float3 from_eye = view * along;
        const float3 direction = on_planet(from_eye, up, faked + eye_height, faked, radius);
        const SkyCell cell = sky_at(direction);
        const float depth = fog_depth(cell, from_height, to_height, to - from);
        const float lowest = min(from_height, to_height);
        from = to;
        from_height = to_height;
        if (depth <= 0.0) {
            continue;
        }
        // Lit as it is where the fog is.
        const float height = clamp(cell.fog_top - (fog_edge * 0.5), lowest, max(lowest, cell.fog_top));
        const float sun_mu = dot(sun, normalize((up * (faked + eye_height)) + from_eye));
        const float3 sunlight = transmittance(air, transmittance_table, height * scale, sun_mu);
        // Under cloud, lit by what comes through it, as the air is.
        const Shade shade = shade_at(lighting, cloud_shadow, from_eye);
        const float3 level_sunlight = sunlight * max(sun_mu, 0.0);
        const float3 lit = mist_light(through_cloud(shade, sunlight), sun_mu, cos_sun,
                                      mist_above(cell, height),
                                      sky_under_cloud(lighting, shade, level_sunlight));
        const float hidden = seen_through * (1.0 - exp(-depth));
        radiance += lit * hidden;
        distance += along * hidden;
        seen_through -= hidden;
    }
    const float opacity = 1.0 - seen_through;
    if (opacity <= 0.0) {
        return float4(0.0);
    }
    const float4 between =
        aerial_perspective(aerial_volume, uv, (distance / opacity) * scale, lighting.reach.x);
    return float4((radiance * between.a) + (between.rgb * opacity), opacity);
}

[shader("fragment")]
float4 fragment_main(Layer layer) : SV_Target {
    const float4 far = mul(push.eye_from_clip, float4(layer.clip, 1.0, 1.0));
    const float3 view = normalize(far.xyz / far.w);
    const float3 up = push.up_height.xyz;
    const float eye_height = push.up_height.w;
    const float faked = push.radii.y;
    const float centre_to_eye = faked + eye_height;
    const float mu = dot(view, up);
    const float2 uv = (layer.clip * 0.5) + 0.5;

    // How far the view reaches: to what's drawn, else to the ground.
    float reach = to_layer(mu, centre_to_eye, eye_height, 0.0, faked);
    if (reach <= 0.0) {
        reach = 1e30;
    }
    const float depth = depth_image.Load(int3(int2(layer.position.xy), 0));
    if (depth > sky_depth) {
        const float4 drawn = mul(push.eye_from_clip, float4(layer.clip, depth, 1.0));
        reach = min(reach, length(drawn.xyz / drawn.w));
    }

    // The clouds marched at half the picture's size, blended from the four nearest, but those of
    // them beyond what's drawn here.
    uint width;
    uint height;
    marched.GetDimensions(width, height);
    uint full_width;
    uint full_height;
    depth_image.GetDimensions(full_width, full_height);
    const float2 at = (layer.position.xy * float2(width, height) / float2(full_width, full_height)) -
                      0.5;
    const float2 corner = floor(at);
    const float2 within = at - corner;
    float4 cloud = float4(0.0);
    for (uint tap = 0; tap < 4; ++tap) {
        const int2 offset = int2(int(tap & 1), int(tap >> 1));
        const int2 texel = clamp(int2(corner) + offset, int2(0), int2(width - 1, height - 1));
        const float2 weights = lerp(1.0 - within, within, float2(offset));
        if (distances.Load(int3(texel, 0)) < reach) {
            cloud += marched.Load(int3(texel, 0)) * (weights.x * weights.y);
        }
    }
    float4 seen = cloud;
    const float fog_top = push.radii.w;
    if (fog_top > 0.0) {
        const float jitter = interleaved(layer.position.xy);
        // Where the view leaves the highest fog's top from under it, or meets it from over it.
        const float shell = to_layer(mu, centre_to_eye, eye_height, fog_top, faked);
        if (eye_height < fog_top) {
            const float end = min(min(reach, shell > 0.0 ? shell : 1e30), fog_reach);
            const float4 fog = fog_between(view, 0.0, end, uv, jitter);
            seen = fog + (cloud * (1.0 - fog.a));
        } else if (shell > 0.0 && shell < reach) {
            const float4 fog =
                fog_between(view, shell, min(reach, shell + far_fog_reach), uv, jitter);
            seen = cloud + (fog * (1.0 - cloud.a));
        }
    }
    if (seen.a <= 0.0) {
        discard;
    }
    return float4(tone_mapped(seen.rgb / seen.a, lighting.sun_exposure.w) * seen.a, seen.a);
}
