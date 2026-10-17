import atmosphere;
import weather;

[[vk::binding(0, 0)]] ConstantBuffer<Air> air;
[[vk::binding(1, 0)]] ConstantBuffer<Lighting> lighting;
[[vk::binding(2, 0)]] Sampler2D transmittance_table;
[[vk::binding(3, 0)]] Sampler2DArray aerial_volume;
[[vk::binding(4, 0)]] Sampler2D<float> depth_image;
// As client::WeatherTexel and client::SkyTexel.
[[vk::binding(5, 0)]] SamplerCube weather_before;
[[vk::binding(6, 0)]] SamplerCube weather_after;
[[vk::binding(7, 0)]] SamplerCube sky_before;
[[vk::binding(8, 0)]] SamplerCube sky_after;
// As render::shape_noise and render::detail_noise.
[[vk::binding(9, 0)]] Sampler3D shape_noise;
[[vk::binding(10, 0)]] Sampler3D detail_noise;
// Premultiplied, before tone mapping; and the metres to the cloud, weighed by what it hides.
[[vk::binding(11, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> marched;
[[vk::binding(12, 0)]] [[vk::image_format("r32f")]] RWTexture2D<float> distances;

struct Push {
    float4x4 eye_from_clip;
    // xyz up through the eye, w its height above sea level.
    float4 up_height;
    // x the planet's radius, y the faked radius drawing bends it onto, z how far the weather has
    // faded from the maps before to those after, w the radians across a pixel of the picture.
    float4 radii;
    // xyz the metres the clouds have drifted, in the planet's frame and within their period.
    float4 moved;
    // Metres above sea level anywhere: x the lowest of the cloud's base, y the highest of its top;
    // z how many frames have been marched since the eye last jumped.
    float4 bounds;
};

[[vk::push_constant]] ConstantBuffer<Push> push;

// As clouds.hpp's.
static const float depth_per_mm = 150.0;
static const float least_cover = 0.05;
// Metres across the shape's and the detail's volumes.
static const float shape_tile = 16000.0;
static const float detail_tile = 2000.0;
static const float shape_texels = 64.0;
static const float detail_texels = 32.0;
// As sky.slang's.
static const float sky_depth = 2e-7;
// The thickest cloud, per metre: as foggy as cloud gets.
static const float densest = 0.1;
// How much further than its share the cover reaches into the billows' thin parts.
static const float overfill = 1.1;
// Steps lengthen with distance, from the shortest to the longest, and are longer through clear air.
static const uint most_steps = 128;
static const float step_growth = 0.03;
static const float clear_stride = 4.0;
static const uint least_steps = 24;
// Of the golden ratio, the part past 1: frame after frame, it spreads offsets evenly.
static const float golden = 0.618034;
static const float shortest_step = 40.0;
static const float longest_step = 2000.0;
// Metres along the view clouds are marched over at most.
static const float march_reach = 100000.0;
// Where the light reaching each step is found, towards the sun, in metres.
static const uint light_steps = 6;
static const float light_reach = 2000.0;
// Scattered forwards strongly, and a little back; the droplets scatter on, not absorb.
static const float forward = 0.8;
static const float backward = -0.3;
static const float backward_share = 0.25;
static const float cloud_albedo = 0.99;
// The weather's looked up a little off from where it's wanted, by a broad noise, so the edges of
// what the cells hold wander, not run straight across them; the noise lifts and lowers the base too.
// A whole number of tiles fits in clouds.hpp's cloud_period.
static const float wander_tile = 32000.0;
static const float wander = 8000.0;
static const float lift = 0.25;

// The weather at a place, as both maps fade between.
struct Cell {
    float cover;
    float mm;
    float base;
    float top;
};

Cell cell_at(float3 direction) {
    const float4 weather = lerp(weather_before.SampleLevel(direction, 0.0),
                                weather_after.SampleLevel(direction, 0.0), push.radii.z);
    const SkyCell sky = sky_cell(lerp(sky_before.SampleLevel(direction, 0.0),
                                      sky_after.SampleLevel(direction, 0.0), push.radii.z));
    Cell cell;
    cell.cover = weather.r;
    cell.mm = weather.g * weather.g * 16.0;
    cell.base = sky.base;
    cell.top = sky.top;
    return cell;
}

// The weather at `direction` on a planet of `radius`, its edges wandering and its base uneven.
Cell wandering_cell_at(float3 direction, float radius) {
    const float3 ground = direction * radius;
    const float4 broad = shape_noise.SampleLevel((ground + push.moved.xyz) / wander_tile, 1.0);
    Cell cell = cell_at(normalize(ground + ((broad.gba - 0.5) * wander)));
    cell.base += (broad.r - 0.5) * lift * (cell.top - cell.base);
    return cell;
}

float remap(float value, float from_low, float from_high, float to_low, float to_high) {
    return to_low + ((value - from_low) * (to_high - to_low) / (from_high - from_low));
}

// How much cloud there is, from none to 1, at `height` above sea level in `cell`, `at` metres in
// the planet's frame; with its edges worn finely when `worn`. `footprint` is the metres a pixel
// spans there. `billows` is left as how much the cloud billows there, from 0 to 1 evenly.
float cloud_at(float3 at, float height, Cell cell, float footprint, bool worn, out float billows) {
    billows = 0.0;
    if (cell.cover < 1.0 / 255.0 || height <= cell.base || height >= cell.top) {
        return 0.0;
    }
    const float thickness = cell.top - cell.base;
    const float up = (height - cell.base) / thickness;
    // Flat layers keep flat; deep clouds round towards their tops.
    const float rounding = lerp(0.1, 0.6, saturate((thickness - 500.0) / 2500.0));
    const float profile = saturate(up / 0.08) * saturate((1.0 - up) / rounding);

    const float3 drifted = at + push.moved.xyz;
    const float4 shape = shape_noise.SampleLevel(
        drifted / shape_tile, max(log2(footprint * shape_texels / shape_tile), 0.0));
    billows = shape.r;
    // The billows are spread evenly, so about as much of the sky's covered as the cover says, and a
    // full cover leaves no gaps. Broken cloud covers less higher up, so it billows into domes.
    const float doming = (1.0 - cell.cover) * smoothstep(0.3, 1.0, up) * 0.9;
    const float covering = cell.cover * (1.0 - doming) * overfill;
    float cloud = saturate(remap(shape.r * profile, 1.0 - covering, 1.0, 0.0, 1.0)) * cell.cover;
    // Broken cloud's worn by the cells, where it's thin.
    const float wearing = (shape.g * 0.625) + (shape.b * 0.25) + (shape.a * 0.125);
    cloud = saturate(remap(cloud, (1.0 - wearing) * 0.25 * (1.0 - cell.cover), 1.0, 0.0, 1.0));
    if (worn && cloud > 0.0) {
        const float detail = detail_noise
                                 .SampleLevel(drifted / detail_tile,
                                              max(log2(footprint * detail_texels / detail_tile),
                                                  0.0))
                                 .a;
        // Wispy below, billowing above; less so the more of the sky's covered, so a full cover
        // keeps no gaps.
        const float eroding = lerp(detail, 1.0 - detail, saturate(up * 4.0));
        cloud = saturate(
            remap(cloud, eroding * 0.3 * (1.0 - (0.8 * cell.cover)), 1.0, 0.0, 1.0));
    }
    return cloud;
}

float thickness_of(Cell cell) {
    return max(cell.top - cell.base, 1.0);
}

// Per metre, how much light the cloud in `cell` takes where it's as thick as it gets.
float densest_in(Cell cell) {
    const float column = depth_per_mm * cell.mm / max(cell.cover, least_cover);
    return min(2.0 * column / thickness_of(cell), densest);
}

float phase(float cos_angle) {
    return lerp(henyey_greenstein(cos_angle, forward), henyey_greenstein(cos_angle, backward),
                backward_share);
}

// Of `view`, where it's within the shell of air any cloud lies in, from the eye: x where it enters
// and y where it leaves, y below x for nowhere.
float2 shell_along(float mu, float centre_to_eye, float eye_height, float faked) {
    const float lowest = push.bounds.x;
    const float highest = push.bounds.y;
    const float2 outer = sphere_hits(mu, centre_to_eye, eye_height, highest, faked);
    if (highest <= lowest || outer.y <= 0.0) {
        return float2(1.0, 0.0);
    }
    float start = eye_height > highest ? outer.x : 0.0;
    float end = outer.y;
    const float2 inner = sphere_hits(mu, centre_to_eye, eye_height, lowest, faked);
    if (eye_height < lowest) {
        start = max(start, inner.y);
    } else if (inner.x > 0.0) {
        end = min(end, inner.x);
    }
    return float2(start, end);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void march_main(uint3 id: SV_DispatchThreadID) {
    uint width;
    uint height;
    marched.GetDimensions(width, height);
    if (id.x >= width || id.y >= height) {
        return;
    }
    const float2 uv = (float2(id.xy) + 0.5) / float2(width, height);
    const float2 clip = (uv * 2.0) - 1.0;
    const float4 far = mul(push.eye_from_clip, float4(clip, 1.0, 1.0));
    const float3 view = normalize(far.xyz / far.w);
    const float3 up = push.up_height.xyz;
    const float eye_height = push.up_height.w;
    const float radius = push.radii.x;
    const float faked = push.radii.y;
    const float centre_to_eye = faked + eye_height;
    const float mu = dot(view, up);

    // How far the view reaches: to the furthest drawn under this texel, else to the ground.
    float reach = to_layer(mu, centre_to_eye, eye_height, 0.0, faked);
    if (reach <= 0.0) {
        reach = 1e30;
    }
    uint full_width;
    uint full_height;
    depth_image.GetDimensions(full_width, full_height);
    float furthest = 1.0;
    for (uint corner = 0; corner < 4; ++corner) {
        const uint2 pixel = min((id.xy * 2) + uint2(corner & 1, corner >> 1),
                                uint2(full_width - 1, full_height - 1));
        furthest = min(furthest, depth_image.Load(int3(int2(pixel), 0)));
    }
    if (furthest > sky_depth) {
        const float4 drawn = mul(push.eye_from_clip, float4(clip, furthest, 1.0));
        reach = min(reach, length(drawn.xyz / drawn.w));
    }

    const float2 shell = shell_along(mu, centre_to_eye, eye_height, faked);
    const float start = shell.x;
    const float end = min(min(shell.y, reach), march_reach);
    if (end <= start) {
        marched[id.xy] = float4(0.0);
        distances[id.xy] = 0.0;
        return;
    }

    const float3 sun = lighting.sun_exposure.xyz;
    const float scale = lighting.sky_scale.w;
    const float cos_sun = dot(view, sun);
    const float pixel = push.radii.w * 2.0;
    float3 radiance = float3(0.0);
    float seen_through = 1.0;
    float weighed = 0.0;
    // No step's longer than a share of the view's way through the shell, so even a thin layer seen
    // from afar is stepped through, not hit or missed whole.
    const float longest = clamp((end - start) / float(least_steps), shortest_step, longest_step);
    // Offset afresh each frame, so frames blended together fill in between the steps.
    const float jitter = frac(interleaved(float2(id.xy)) + (push.bounds.z * golden));
    float along = start + (jitter * clamp(start * step_growth, shortest_step, longest));
    // Whether the last step was a long one through clear air, and how far to go only in steps once
    // one strode into cloud.
    bool striding = false;
    float stepping_to = 0.0;
    for (uint at = 0; at < most_steps && along < end && seen_through > 0.01; ++at) {
        const float step = clamp(along * step_growth, shortest_step, longest);
        const float3 from_eye = view * along;
        const float point_height = drawn_height(from_eye, up, eye_height, faked);
        const float3 direction = on_planet(from_eye, up, centre_to_eye, faked, radius);
        const Cell cell = wandering_cell_at(direction, radius);
        const float3 point = direction * (radius + point_height);
        const float footprint = along * pixel;
        float billows = 0.0;
        const float cloud = cloud_at(point, point_height, cell, footprint, true, billows);
        if (cloud > 0.0 && striding) {
            // Strode into cloud: back to a step past where it was clear, and on in steps.
            stepping_to = along;
            along -= step * (clear_stride - 1.0);
            striding = false;
            continue;
        }
        if (cloud > 0.0) {
            const float extinction = cloud * densest_in(cell);
            const float3 point_up = normalize((up * centre_to_eye) + from_eye);
            const float sun_mu = dot(point_up, sun);
            // What the cloud towards the sun takes, as if the weather there were the same.
            float towards_sun = 0.0;
            float before = 0.0;
            float unused = 0.0;
            for (uint light = 0; light < light_steps; ++light) {
                const float share = float(light + 1) / float(light_steps);
                const float after = light_reach * share * share;
                const float middle = (before + after) * 0.5;
                const float3 lit_at = point + (sun * middle);
                const float lit_height = point_height + (middle * sun_mu);
                towards_sun += cloud_at(lit_at, lit_height, cell, footprint, false, unused) *
                               densest_in(cell) * (after - before);
                before = after;
            }
            const float3 sunlight =
                transmittance(air, transmittance_table, point_height * scale, sun_mu);
            // Light scattered many times, as fainter, wider-spread and less hidden octaves.
            float scattered = 0.0;
            float strength = 1.0;
            for (uint octave = 0; octave < 3; ++octave) {
                scattered += strength * exp(-towards_sun * strength) *
                             lerp(1.0 / (4.0 * pi), phase(cos_sun), strength);
                strength *= 0.5;
            }
            // The sun's and the sky's light, scattered on through the cloud over the point, as a
            // layer of strongly forward scattering droplets lets it through: as bright as what
            // falls on the top at the top, dimmer deeper down and under where it billows. About as
            // deep as the whole column at the base, as clouds.cpp shades the ground beneath.
            const float column = depth_per_mm * cell.mm / max(cell.cover, least_cover);
            const float over = column * saturate((cell.top - point_height) / thickness_of(cell)) *
                               (0.5 + billows);
            const float let_through = 1.0 / (1.0 + (0.75 * (1.0 - forward) * over));
            const float3 falling = (sunlight * max(sun_mu, 0.0)) + lighting.sky_scale.rgb;
            const float3 lit =
                cloud_albedo * ((sunlight * scattered) + (falling * (let_through / pi)));
            const float hidden = seen_through * (1.0 - exp(-extinction * step));
            radiance += lit * hidden;
            weighed += along * hidden;
            seen_through -= hidden;
        }
        striding = cloud <= 0.0 && along >= stepping_to;
        along += striding ? step * clear_stride : step;
    }
    const float opacity = 1.0 - seen_through;
    if (opacity <= 0.0) {
        marched[id.xy] = float4(0.0);
        distances[id.xy] = 0.0;
        return;
    }
    const float distance = weighed / opacity;
    const float4 between = aerial_perspective(aerial_volume, uv, distance * scale, lighting.reach.x);
    marched[id.xy] = float4((radiance * between.a) + (between.rgb * opacity), opacity);
    distances[id.xy] = distance;
}
