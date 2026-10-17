import atmosphere;
import tone;
import weather;

[[vk::binding(0, 0)]] ConstantBuffer<Air> air;
[[vk::binding(1, 0)]] ConstantBuffer<Lighting> lighting;
[[vk::binding(2, 0)]] Sampler2D transmittance_table;
[[vk::binding(3, 0)]] Sampler2DArray aerial_volume;
[[vk::binding(4, 0)]] Sampler2D<float> depth_image;
// Each texel: cover, the root of a 16th of the cloud's mm, the root of a 256th of mm falling an
// hour, and the share falling as snow.
[[vk::binding(5, 0)]] SamplerCube weather_before;
[[vk::binding(6, 0)]] SamplerCube weather_after;
// As client::SkyTexel.
[[vk::binding(7, 0)]] SamplerCube sky_before;
[[vk::binding(8, 0)]] SamplerCube sky_after;
// As cloud_march.slang leaves them.
[[vk::binding(9, 0)]] Sampler2D marched;
[[vk::binding(10, 0)]] Sampler2D<float> distances;

struct Push {
    float4x4 eye_from_clip;
    // xyz up through the eye, w its height above sea level.
    float4 up_height;
    // x the planet's radius, y the faked radius drawing bends it onto, z the base of the cloud over
    // the eye, w how far the weather has faded from the maps before to those after.
    float4 radii;
    // x 1 when anything falls anywhere in the weather, else 0; metres above sea level anywhere: y
    // the highest fog's top, 0 for none, and z the highest of the cloud's base.
    float4 veil_fog;
};

[[vk::push_constant]] ConstantBuffer<Push> push;

// Nothing drawn is behind the sky, which sky.slang draws at 1e-7.
static const float sky_depth = 2e-7;
// As clouds.cpp's. Optical depth per mm of liquid water, for droplets of 10 μm.
static const float depth_per_mm = 150.0;
static const float scattered_forward = 0.8;
static const float least_cover = 0.05;
// As clouds.cpp's. Per metre of rain falling at 1 mm an hour; snow, slower and in larger flakes,
// hides more.
static const float rain_extinction = 2.5e-4;
static const float rain_extinction_power = 0.63;
static const float snow_extinction = 12.0;
// Of light falling on what falls, the share scattered on, as if evenly.
static const float rain_albedo = 0.6;
static const float snow_albedo = 0.9;
static const uint veil_steps = 12;
// Metres along the view what falls is seen through, at most.
static const float veil_reach = 40000.0;
static const uint fog_steps = 24;
// Metres of fog marched through at most, past which it hides everything.
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

float4 weather_at(float3 direction) {
    return lerp(weather_before.SampleLevel(direction, 0.0),
                weather_after.SampleLevel(direction, 0.0), push.radii.w);
}

SkyCell sky_at(float3 direction) {
    return sky_cell(lerp(sky_before.SampleLevel(direction, 0.0),
                         sky_after.SampleLevel(direction, 0.0), push.radii.w));
}

// As clouds.cpp's cloud_shade: of the light falling on a layer's top, x the share let straight
// through and y the share scattered through.
float2 shade_of(float cover, float cloud_mm) {
    if (cover <= 0.0) {
        return float2(1.0, 0.0);
    }
    const float thickness = depth_per_mm * cloud_mm / max(cover, least_cover);
    const float unscattered = exp(-thickness);
    const float let_through = 1.0 / (1.0 + (0.75 * (1.0 - scattered_forward) * thickness));
    return float2((1.0 - cover) + (cover * unscattered), cover * (let_through - unscattered));
}

// As clouds.cpp's rain_extinction.
float extinction_of(float falling, float snow) {
    return falling > 0.0 ? rain_extinction * pow(falling, rain_extinction_power) *
                               (1.0 + ((snow_extinction - 1.0) * snow))
                         : 0.0;
}

// What falls beneath the cloud, from `start` to `end` along the view, premultiplied, before tone
// mapping. Each step is lit as under the cloud above it, and the steps are offset by `jitter`, so
// the banding of too few becomes a finer noise.
float4 veil_between(float3 view, float start, float end, float2 uv, float jitter) {
    const float3 up = push.up_height.xyz;
    const float radius = push.radii.x;
    const float faked = push.radii.y;
    const float centre_to_eye = faked + push.up_height.w;
    const float3 sun = lighting.sun_exposure.xyz;
    const float scale = lighting.sky_scale.w;

    const float step = (end - start) / float(veil_steps);
    float3 radiance = float3(0.0);
    float seen_through = 1.0;
    float distance = 0.0;
    for (uint at = 0; at < veil_steps; ++at) {
        const float along = start + ((float(at) + jitter) * step);
        const float3 from_eye = view * along;
        const float3 drawn_up = (up * centre_to_eye) + from_eye;
        const float height = length(drawn_up) - faked;
        if (height < 0.0) {
            continue;
        }
        const float3 direction = on_planet(from_eye, up, centre_to_eye, faked, radius);
        if (height > sky_at(direction).base) {
            continue;
        }
        const float4 weather = weather_at(direction);
        const float extinction = extinction_of(weather.b * weather.b * 256.0, weather.a);
        if (extinction <= 0.0) {
            continue;
        }
        const float2 shade = shade_of(weather.r, weather.g * weather.g * 16.0);
        const float sun_mu = dot(sun, drawn_up) / (faked + height);
        const float3 level_sunlight =
            transmittance(air, transmittance_table, height * scale, sun_mu) * max(sun_mu, 0.0);
        // The sun's and the sky's light alike, as much as the cloud above lets through.
        const float3 lit = (level_sunlight + lighting.sky_scale.rgb) * (shade.x + shade.y);
        const float albedo = lerp(rain_albedo, snow_albedo, weather.a);
        const float hidden = seen_through * (1.0 - exp(-extinction * step));
        radiance += lit * (albedo / pi) * hidden;
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

// The fog from `start` to `end` along the view, premultiplied, before tone mapping. Steps are
// finest at the start, where the fog's nearest, and offset by `jitter`.
float4 fog_between(float3 view, float start, float end, float2 uv, float jitter) {
    const float3 up = push.up_height.xyz;
    const float eye_height = push.up_height.w;
    const float radius = push.radii.x;
    const float faked = push.radii.y;
    const float3 sun = lighting.sun_exposure.xyz;
    const float scale = lighting.sky_scale.w;
    const float cos_sun = dot(view, sun);
    // Under cloud, lit by what comes through it, as the air is.
    const float shaded = lighting.reach.y + lighting.reach.z;

    const float length = end - start;
    float3 radiance = float3(0.0);
    float seen_through = 1.0;
    float distance = 0.0;
    for (uint at = 0; at < fog_steps; ++at) {
        const float from = float(at) / float(fog_steps);
        const float to = float(at + 1) / float(fog_steps);
        const float t = (float(at) + jitter) / float(fog_steps);
        const float along = start + (length * t * t);
        const float step = length * ((to * to) - (from * from));
        const float3 from_eye = view * along;
        const float height = drawn_height(from_eye, up, eye_height, faked);
        const float3 direction = on_planet(from_eye, up, faked + eye_height, faked, radius);
        const SkyCell cell = sky_at(direction);
        const float extinction = fog_extinction(cell, height);
        if (extinction <= 0.0) {
            continue;
        }
        const float sun_mu = dot(sun, normalize((up * (faked + eye_height)) + from_eye));
        const float3 sunlight = transmittance(air, transmittance_table, height * scale, sun_mu);
        const float3 lit = mist_light(sunlight, sun_mu, cos_sun, mist_above(cell, height),
                                      lighting.sky_scale.rgb) *
                           shaded;
        const float hidden = seen_through * (1.0 - exp(-extinction * step));
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
    const float base = push.radii.z;
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
    float4 veil = float4(0.0);
    if (push.veil_fog.x > 0.0) {
        const float jitter = interleaved(layer.position.xy);
        if (eye_height < base) {
            veil = veil_between(view, 0.0, min(reach, veil_reach), uv, jitter);
        } else {
            // From where the view meets the highest of the cloud's base.
            const float start = to_layer(mu, centre_to_eye, eye_height, push.veil_fog.z, faked);
            if (start > 0.0 && start < reach) {
                veil = veil_between(view, start, min(reach, start + veil_reach), uv, jitter);
            }
        }
    }
    // Premultiplied, the nearer over the further, over what's drawn beyond.
    const float4 overhead =
        eye_height < base ? veil + (cloud * (1.0 - veil.a)) : cloud + (veil * (1.0 - cloud.a));
    float4 seen = overhead;
    const float fog_top = push.veil_fog.y;
    if (fog_top > 0.0) {
        const float jitter = interleaved(layer.position.xy + 17.0);
        // Where the view leaves the highest fog's top from under it, or meets it from over it.
        const float shell = to_layer(mu, centre_to_eye, eye_height, fog_top, faked);
        if (eye_height < fog_top) {
            const float end = min(min(reach, shell > 0.0 ? shell : 1e30), fog_reach);
            const float4 fog = fog_between(view, 0.0, end, uv, jitter);
            seen = fog + (overhead * (1.0 - fog.a));
        } else if (shell > 0.0 && shell < reach) {
            const float4 fog = fog_between(view, shell, min(reach, shell + fog_reach), uv, jitter);
            seen = overhead + (fog * (1.0 - overhead.a));
        }
    }
    if (seen.a <= 0.0) {
        discard;
    }
    return float4(tone_mapped(seen.rgb / seen.a, lighting.sun_exposure.w) * seen.a, seen.a);
}
