import atmosphere;
import tone;

[[vk::binding(0, 0)]] ConstantBuffer<Air> air;
[[vk::binding(1, 0)]] ConstantBuffer<Lighting> lighting;
[[vk::binding(2, 0)]] Sampler2D transmittance_table;
[[vk::binding(3, 0)]] Sampler2DArray aerial_volume;
[[vk::binding(4, 0)]] Sampler2D<float> depth_image;
// Each texel: cover, the root of a 16th of the cloud's mm, the root of a 256th of mm falling an
// hour, and the share falling as snow.
[[vk::binding(5, 0)]] SamplerCube weather_before;
[[vk::binding(6, 0)]] SamplerCube weather_after;

struct Push {
    float4x4 eye_from_clip;
    // xyz up through the eye, w its height above sea level.
    float4 up_height;
    // x the planet's radius, y the faked radius drawing bends it onto, z the layer's height above
    // sea level, w how far the weather has faded from the map before to the one after.
    float4 radii;
    // xyz the metres the clouds have drifted, in the planet's frame and within the noise's period;
    // w the radians across a pixel.
    float4 moved_pixel;
};

[[vk::push_constant]] ConstantBuffer<Push> push;

// As clouds.hpp's.
static const float feature = 4000.0;
static const int noise_period = 256;
static const uint octaves = 5;
// Nothing drawn is behind the sky, which sky.slang draws at 1e-7.
static const float sky_depth = 2e-7;
// Of a covered patch's cover, how much is its soft edge.
static const float edge = 0.2;
// Optical depth per mm of liquid water, for droplets of 10 μm.
static const float depth_per_mm = 150.0;
static const float scattered_forward = 0.8;

struct Layer {
    float4 position : SV_Position;
    float2 clip;
};

uint hashed(int3 at) {
    const uint3 wrapped = uint3(((at % noise_period) + noise_period) % noise_period);
    uint h = (wrapped.x * 73856093u) ^ (wrapped.y * 19349663u) ^ (wrapped.z * 83492791u);
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return h;
}

float lattice(int3 at) {
    return float(hashed(at) & 0xffffu) / 65535.0;
}

float value_noise(float3 at) {
    const float3 cell = floor(at);
    const float3 f = at - cell;
    const float3 u = f * f * (3.0 - (2.0 * f));
    const int3 c = int3(cell);
    const float x00 = lerp(lattice(c), lattice(c + int3(1, 0, 0)), u.x);
    const float x10 = lerp(lattice(c + int3(0, 1, 0)), lattice(c + int3(1, 1, 0)), u.x);
    const float x01 = lerp(lattice(c + int3(0, 0, 1)), lattice(c + int3(1, 0, 1)), u.x);
    const float x11 = lerp(lattice(c + int3(0, 1, 1)), lattice(c + int3(1, 1, 1)), u.x);
    return lerp(lerp(x00, x10, u.y), lerp(x01, x11, u.y), u.z);
}

// From 0 to 1, repeating every noise_period features; octaves finer than `detail` features a
// pixel fade to their mean, so far clouds don't shimmer.
float billows(float3 at, float detail) {
    float sum = 0.0;
    float amplitude = 0.5;
    float total = 0.0;
    float frequency = 1.0;
    for (uint octave = 0; octave < octaves; ++octave) {
        const float kept = saturate((detail / frequency) - 1.0);
        if (kept == 0.0) {
            break;
        }
        sum += amplitude * lerp(0.5, value_noise(at * frequency), kept);
        total += amplitude;
        amplitude *= 0.5;
        frequency *= 2.0;
    }
    return total > 0.0 ? sum / total : 0.5;
}

// How much of cloud is at `point`, metres from the planet's centre, where the weather covers
// `cover` of the sky: none to full.
float density_at(float3 point, float cover, float detail) {
    const float3 at = (point + push.moved_pixel.xyz) / feature;
    // Summed octaves bunch about the middle; spread them back out, so cover is about the share
    // covered.
    const float spread = saturate(((billows(at, detail) - 0.5) * 2.5) + 0.5);
    return saturate(((cover * (1.0 + edge)) - (1.0 - spread)) / edge);
}

// Henyey-Greenstein's.
float phase(float cos_angle, float g) {
    const float g2 = g * g;
    return (1.0 - g2) / (4.0 * pi * pow(1.0 + g2 - (2.0 * g * cos_angle), 1.5));
}

// The distance along `view` to the drawn sphere of radius `faked` + `base` about the drawn
// planet's centre, `centre_to_eye` below the eye: the nearest ahead, or negative for none.
float to_layer(float mu, float centre_to_eye, float eye_height, float base, float faked) {
    // The roots' product, found without cancelling.
    const float product = (eye_height - base) * (eye_height + base + (2.0 * faked));
    const float b = -mu * centre_to_eye;
    const float discriminant = (b * b) - product;
    if (discriminant < 0.0) {
        return -1.0;
    }
    const float q = b + (b >= 0.0 ? sqrt(discriminant) : -sqrt(discriminant));
    if (q == 0.0) {
        return -1.0;
    }
    const float first = min(q, product / q);
    const float second = max(q, product / q);
    return first > 0.0 ? first : second;
}

// A triangle covering the whole picture.
[shader("vertex")]
Layer vertex_main(uint id: SV_VertexID) {
    const float2 clip = (float2((id << 1) & 2, id & 2) * 2.0) - 1.0;
    Layer layer;
    layer.position = float4(clip, 0.0, 1.0);
    layer.clip = clip;
    return layer;
}

[shader("fragment")]
float4 fragment_main(Layer layer) : SV_Target {
    const float4 far = mul(push.eye_from_clip, float4(layer.clip, 1.0, 1.0));
    const float3 view = normalize(far.xyz / far.w);
    const float3 up = push.up_height.xyz;
    const float eye_height = push.up_height.w;
    const float radius = push.radii.x;
    const float faked = push.radii.y;
    const float base = push.radii.z;
    const float centre_to_eye = faked + eye_height;

    const float along = to_layer(dot(view, up), centre_to_eye, eye_height, base, faked);
    if (along <= 0.0) {
        discard;
    }
    const float depth = depth_image.Load(int3(int2(layer.position.xy), 0));
    if (depth > sky_depth) {
        const float4 drawn = mul(push.eye_from_clip, float4(layer.clip, depth, 1.0));
        if (length(drawn.xyz / drawn.w) < along) {
            discard;
        }
    }

    // Where on the planet that is: as far along the ground as drawn, turned about the true centre.
    const float3 hit = view * along;
    const float3 drawn_up = (up * centre_to_eye) + hit;
    const float3 across = hit - (up * dot(hit, up));
    const float out = length(across);
    const float turned = atan2(out, dot(drawn_up, up)) * (faked / radius);
    const float3 direction =
        (up * cos(turned)) + (out > 0.0 ? across * (sin(turned) / out) : float3(0.0));
    const float4 weather =
        lerp(weather_before.SampleLevel(direction, 0.0), weather_after.SampleLevel(direction, 0.0),
             push.radii.w);
    const float cover = weather.r;
    if (cover < 1.0 / 255.0) {
        discard;
    }

    const float3 normal = normalize(drawn_up);
    const float facing = max(abs(dot(view, normal)), 0.05);
    const float detail = feature * facing / (along * push.moved_pixel.w);
    const float shape = density_at(direction * (radius + base), cover, detail);
    if (shape <= 0.0) {
        discard;
    }
    const float cloud_mm = weather.g * weather.g * 16.0;
    const float thickness = depth_per_mm * (cloud_mm / max(cover, 0.05)) * shape;
    const float through = thickness / facing;

    const float3 sun = lighting.sun_exposure.xyz;
    const float scale = lighting.sky_scale.w;
    const float sun_mu = dot(sun, normal);
    const float3 sunlight = transmittance(air, transmittance_table, base * scale, sun_mu);
    const float3 falling = (sunlight * max(sun_mu, 0.0)) + lighting.sky_scale.rgb;
    // Of light falling on its top, the share a layer lets through: diffusely, as a layer of
    // strongly forward scattering droplets does.
    const float let_through = 1.0 / (1.0 + (0.75 * (1.0 - scattered_forward) * thickness));
    float3 radiance;
    if (eye_height < base) {
        // Thin cloud before the sun shines with what's scattered just off it.
        radiance = (falling * (let_through / pi)) +
                   (sunlight * (phase(dot(view, sun), scattered_forward) * through *
                                exp(-through)));
    } else {
        radiance = falling * ((1.0 - let_through) / pi);
    }

    const float2 uv = (layer.clip * 0.5) + 0.5;
    const float4 between = aerial_perspective(aerial_volume, uv, along * scale, lighting.reach.x);
    const float opacity = shape * (1.0 - exp(-through));
    const float3 seen = (radiance * between.a) + between.rgb;
    // Premultiplied, over what's drawn beyond.
    return float4(tone_mapped(seen, lighting.sun_exposure.w) * opacity, opacity);
}
