import atmosphere;
import shadow;
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
// The cloud's shadow, as shadow_main() leaves it in `shadow_map`: of the light along the sun's to
// the ground through each texel at the height the map's laid at, r the share let straight through
// the cloud; g and b, of the light falling on the cloud over the ground there, as clouds.hpp's
// cloud_shade has it, the shares let straight through and scattered through; and a the metres
// above sea level of the top of the cloud the sun's light goes through, or over the ground.
[[vk::binding(13, 0)]] Sampler2D cloud_shadow;
[[vk::binding(14, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> shadow_map;

struct Push {
    float4x4 eye_from_clip;
    // xyz up through the eye, w its height above sea level.
    float4 up_height;
    // x the planet's radius, y the faked radius drawing bends it onto, z how far the weather has
    // faded from the maps before to those after, w the radians across a marched texel.
    float4 radii;
    // xyz the metres the clouds have drifted, in the planet's frame and within their period; w how
    // many of the picture's pixels a marched texel spans, across and down.
    float4 moved;
    // Metres above sea level anywhere: x the lowest of the cloud's base, y the highest of its top,
    // and w the highest of its base, or -1 when nothing falls anywhere; z how many frames have been
    // marched since the eye last jumped.
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
// Steps lengthen with distance, from the shortest to the longest; enough of them to reach across
// the whole shell the clouds lie in, near the horizon, without leaps that would skip clouds whole.
static const uint most_steps = 192;
static const float step_growth = 0.03;
// Through cloud, no step's deeper than this, as the light it takes, nor shorter than a pixel's
// footprint or a share of the step it'd otherwise be; so where cloud starts, which the step it's
// found in would round to, is found as finely as it's seen.
static const float step_depth = 1.0;
static const float most_refined = 4.0;
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
// As clouds.hpp's. Per metre of rain falling at 1 mm an hour; snow, slower and in larger flakes,
// hides more.
static const float rain_extinction = 2.5e-4;
static const float rain_extinction_power = 0.63;
static const float snow_extinction = 12.0;
// Of light falling on what falls, the share scattered on, as if evenly.
static const float rain_albedo = 0.6;
static const float snow_albedo = 0.9;
static const uint rain_steps = 16;
// Metres along the view what falls is seen through, at most.
static const float rain_reach = 40000.0;
// Rain falls from the cloud as it is this far up it, as a share of its depth, and from within its
// edges: where its density, as a share of its cover, is past the least, fully past the most. It
// thickens over this many metres below the base, so the shafts have no flat tops.
static const float rain_from = 0.1;
static const float rain_least = 0.15;
static const float rain_most = 0.6;
static const float rain_top = 300.0;
// Metres a pixel's taken to span, at least, where the cloud's found for rain: its shafts are broad.
static const float rain_footprint = 1000.0;
// It falls in streaks, this many metres across and stretched this many times as long.
static const float streak_tile = 1000.0;
static const float streak_stretch = 10.0;
// Of the cloud's shadow, metres along the sun's light per step at least, and steps at most and
// least.
static const float shadow_step = 300.0;
static const uint most_shadow_steps = 64;
static const uint least_shadow_steps = 8;

// The weather at a place, as both maps fade between.
struct Cell {
    float cover;
    float mm;
    float base;
    float top;
    // mm an hour, and the share of it as snow.
    float falling;
    float snow;
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
    cell.falling = weather.b * weather.b * 256.0;
    cell.snow = weather.a;
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

// As clouds.hpp's rain_extinction.
float extinction_of(float falling, float snow) {
    return falling > 0.0 ? rain_extinction * pow(falling, rain_extinction_power) *
                               (1.0 + ((snow_extinction - 1.0) * snow))
                         : 0.0;
}

// As clouds.hpp's cloud_shade: x the share let straight through, y scattered through.
float2 cloud_shade(Cell cell) {
    if (cell.cover <= 0.0) {
        return float2(1.0, 0.0);
    }
    const float column = depth_per_mm * cell.mm / max(cell.cover, least_cover);
    const float unscattered = exp(-column);
    const float let_through = 1.0 / (1.0 + (0.75 * (1.0 - forward) * column));
    return float2((1.0 - cell.cover) + (cell.cover * unscattered),
                  cell.cover * (let_through - unscattered));
}

// How heavily it falls in its streaks at `height` over `direction`, about 1 on the whole.
float streaks_at(float3 direction, float height, float footprint) {
    const float3 at = (direction * (push.radii.x + (height / streak_stretch))) + push.moved.xyz;
    const float detail = detail_noise
                             .SampleLevel(at / streak_tile,
                                          max(log2(footprint * detail_texels / streak_tile), 0.0))
                             .a;
    return 0.25 + (1.5 * detail);
}

// As clouds.hpp's rain_beneath.
float rain_beneath(float cover, float presence) {
    return lerp(1.0, presence / max(cover, least_cover), saturate(cover / least_cover));
}

// How much the cloud in `cell` is there over `direction`, from 0 in its gaps to 1 under its
// billows, as rain falls from it.
float raining_from(float3 direction, Cell cell, float footprint) {
    const float height = lerp(cell.base, cell.top, rain_from);
    float unused = 0.0;
    const float cloud = cloud_at(direction * (push.radii.x + height), height, cell,
                                 max(footprint, rain_footprint), false, unused);
    return saturate(
        remap(cloud / max(cell.cover, 1.0 / 255.0), rain_least, rain_most, 0.0, 1.0));
}

// The view's way through the sky, and the picture's texel it's for.
struct Ray {
    float3 view;
    float2 uv;
    // How far it reaches: to what's drawn, else to the ground.
    float reach;
    // Offsets each step, afresh each frame, so frames blended together fill in between the steps.
    float jitter;
};

// What falls beneath the cloud along `ray`, from `start` to `end`, premultiplied, before tone
// mapping; `distance` is left as the metres to it, weighed by what it hides. It falls in streaks
// beneath the billows over it, not the gaps between, and each step's lit as the cloud's shadow
// has it. Steps are finest at the start.
float4 rain_between(Ray ray, float start, float end, out float distance) {
    const float3 up = push.up_height.xyz;
    const float eye_height = push.up_height.w;
    const float radius = push.radii.x;
    const float faked = push.radii.y;
    const float centre_to_eye = faked + eye_height;
    const float3 sun = lighting.sun_exposure.xyz;
    const float scale = lighting.sky_scale.w;
    const float pixel = push.radii.w;

    const float span = end - start;
    float3 radiance = float3(0.0);
    float seen_through = 1.0;
    float weighed = 0.0;
    float from = start;
    for (uint at = 0; at < rain_steps; ++at) {
        const float share = float(at + 1) / float(rain_steps);
        const float to = start + (span * share * share);
        const float along = lerp(from, to, ray.jitter);
        const float step = to - from;
        from = to;
        const float3 from_eye = ray.view * along;
        const float height = drawn_height(from_eye, up, eye_height, faked);
        if (height < 0.0) {
            continue;
        }
        const float3 direction = on_planet(from_eye, up, centre_to_eye, faked, radius);
        const Cell cell = wandering_cell_at(direction, radius);
        if (height >= cell.base || cell.falling <= 0.0) {
            continue;
        }
        const float footprint = along * pixel;
        const float presence = raining_from(direction, cell, footprint);
        const float extinction = extinction_of(cell.falling, cell.snow) *
                                 rain_beneath(cell.cover, presence) *
                                 streaks_at(direction, height, footprint) *
                                 saturate((cell.base - height) / rain_top);
        if (extinction <= 0.0) {
            continue;
        }
        const float sun_mu = dot(sun, normalize((up * centre_to_eye) + from_eye));
        const float3 level_sunlight =
            transmittance(air, transmittance_table, height * scale, sun_mu) * max(sun_mu, 0.0);
        const Shade shade = shade_at(lighting, cloud_shadow, from_eye);
        const float3 lit = through_cloud(shade, level_sunlight) +
                           sky_under_cloud(lighting, shade, level_sunlight);
        const float albedo = lerp(rain_albedo, snow_albedo, cell.snow);
        const float hidden = seen_through * (1.0 - exp(-extinction * step));
        radiance += lit * (albedo / pi) * hidden;
        weighed += along * hidden;
        seen_through -= hidden;
    }
    const float opacity = 1.0 - seen_through;
    distance = opacity > 0.0 ? weighed / opacity : 0.0;
    if (opacity <= 0.0) {
        return float4(0.0);
    }
    const float4 between =
        aerial_perspective(aerial_volume, ray.uv, distance * scale, lighting.reach.x);
    return float4((radiance * between.a) + (between.rgb * opacity), opacity);
}

// The cloud along `ray`, from `start` to `end`, premultiplied, before tone mapping; `distance` is
// left as the metres to it, weighed by what it hides.
float4 cloud_between(Ray ray, float start, float end, out float distance) {
    const float3 view = ray.view;
    const float3 up = push.up_height.xyz;
    const float eye_height = push.up_height.w;
    const float radius = push.radii.x;
    const float faked = push.radii.y;
    const float centre_to_eye = faked + eye_height;
    const float3 sun = lighting.sun_exposure.xyz;
    const float scale = lighting.sky_scale.w;
    const float cos_sun = dot(view, sun);
    const float pixel = push.radii.w;
    float3 radiance = float3(0.0);
    float seen_through = 1.0;
    float weighed = 0.0;
    // No step's longer than a share of the view's way through the shell, so even a thin layer seen
    // from afar is stepped through, not hit or missed whole.
    const float longest = clamp((end - start) / float(least_steps), shortest_step, longest_step);
    float along = start + (ray.jitter * clamp(start * step_growth, shortest_step, longest));
    // Where it was last clear, and whether at the last step; and once stepped into cloud, how far
    // to go back over in steps as fine as `refined`.
    float clear_at = start;
    bool was_clear = true;
    float stepping_to = 0.0;
    float refined = 0.0;
    for (uint at = 0; at < most_steps && along < end && seen_through > 0.01; ++at) {
        const float coarse = clamp(along * step_growth, shortest_step, longest);
        const float3 from_eye = view * along;
        const float point_height = drawn_height(from_eye, up, eye_height, faked);
        const float3 direction = on_planet(from_eye, up, centre_to_eye, faked, radius);
        const Cell cell = wandering_cell_at(direction, radius);
        const float3 point = direction * (radius + point_height);
        const float footprint = along * pixel;
        float billows = 0.0;
        const float cloud = cloud_at(point, point_height, cell, footprint, true, billows);
        const float extinction = cloud * densest_in(cell);
        const float fine = clamp(step_depth / max(extinction, 1e-6),
                                 max(footprint, coarse / most_refined), coarse);
        if (cloud > 0.0 && was_clear && along - clear_at > fine * 1.01) {
            // Stepped into cloud: back to where it was clear, and on in finer steps.
            stepping_to = along;
            refined = fine;
            along = clear_at + (refined * ray.jitter);
            continue;
        }
        const float step = cloud > 0.0 ? fine : (along < stepping_to ? refined : coarse);
        if (cloud > 0.0) {
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
        was_clear = cloud <= 0.0;
        if (was_clear) {
            clear_at = along;
        }
        along += step;
    }
    const float opacity = 1.0 - seen_through;
    distance = opacity > 0.0 ? weighed / opacity : 0.0;
    if (opacity <= 0.0) {
        return float4(0.0);
    }
    const float4 between =
        aerial_perspective(aerial_volume, ray.uv, distance * scale, lighting.reach.x);
    return float4((radiance * between.a) + (between.rgb * opacity), opacity);
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
    Ray ray;
    ray.uv = (float2(id.xy) + 0.5) / float2(width, height);
    const float2 clip = (ray.uv * 2.0) - 1.0;
    const float4 far = mul(push.eye_from_clip, float4(clip, 1.0, 1.0));
    ray.view = normalize(far.xyz / far.w);
    ray.jitter = frac(interleaved(float2(id.xy)) + (push.bounds.z * golden));
    const float3 up = push.up_height.xyz;
    const float eye_height = push.up_height.w;
    const float faked = push.radii.y;
    const float centre_to_eye = faked + eye_height;
    const float mu = dot(ray.view, up);

    // How far the view reaches: to the furthest drawn under this texel, else to the ground.
    ray.reach = to_layer(mu, centre_to_eye, eye_height, 0.0, faked);
    if (ray.reach <= 0.0) {
        ray.reach = 1e30;
    }
    uint full_width;
    uint full_height;
    depth_image.GetDimensions(full_width, full_height);
    const uint spans = uint(push.moved.w);
    float furthest = 1.0;
    for (uint under = 0; under < spans * spans; ++under) {
        const uint2 pixel = min((id.xy * spans) + uint2(under % spans, under / spans),
                                uint2(full_width - 1, full_height - 1));
        furthest = min(furthest, depth_image.Load(int3(int2(pixel), 0)));
    }
    if (furthest > sky_depth) {
        const float4 drawn = mul(push.eye_from_clip, float4(clip, furthest, 1.0));
        ray.reach = min(ray.reach, length(drawn.xyz / drawn.w));
    }

    float4 cloud = float4(0.0);
    float cloud_distance = 0.0;
    const float2 shell = shell_along(mu, centre_to_eye, eye_height, faked);
    const float end = min(min(shell.y, ray.reach), march_reach);
    if (end > shell.x) {
        cloud = cloud_between(ray, shell.x, end, cloud_distance);
    }
    float4 rain = float4(0.0);
    float rain_distance = 0.0;
    const float highest_base = push.bounds.w;
    if (highest_base >= 0.0) {
        // From the eye, or from where the view meets the highest of the cloud's base.
        const float start =
            eye_height < highest_base
                ? 0.0
                : to_layer(mu, centre_to_eye, eye_height, highest_base, faked);
        if (start >= 0.0 && start < ray.reach) {
            rain = rain_between(ray, start, min(ray.reach, start + rain_reach), rain_distance);
        }
    }

    // Premultiplied, the nearer over the further.
    const bool rain_nearer = cloud.a <= 0.0 || (rain.a > 0.0 && rain_distance < cloud_distance);
    const float4 nearer = rain_nearer ? rain : cloud;
    const float4 further = rain_nearer ? cloud : rain;
    const float nearer_distance = rain_nearer ? rain_distance : cloud_distance;
    const float further_distance = rain_nearer ? cloud_distance : rain_distance;
    const float4 seen = nearer + (further * (1.0 - nearer.a));
    marched[id.xy] = seen;
    distances[id.xy] = seen.a > 0.0 ? ((nearer.a * nearer_distance) +
                                       (further.a * (1.0 - nearer.a) * further_distance)) /
                                          seen.a
                                    : 0.0;
}

// The cloud's shadow round the eye, as cloud_shadow holds it, over as much of the ground as the
// frame's Lighting has it reach.
[shader("compute")]
[numthreads(8, 8, 1)]
void shadow_main(uint3 id: SV_DispatchThreadID) {
    uint width;
    uint height;
    shadow_map.GetDimensions(width, height);
    if (id.x >= width || id.y >= height) {
        return;
    }
    const float radius = push.radii.x;
    const float reach = lighting.shadow.x;
    const float laid_at = lighting.shadow.y;
    const float3 sun = lighting.sun_exposure.xyz;
    const float texel = 2.0 * reach / float(width);
    const float2 offset = ((float2(id.xy) + 0.5) * texel) - reach;
    const float3 ground = shadow_direction(offset, push.up_height.xyz, radius);
    const Cell over = wandering_cell_at(ground, radius);
    const float2 shade = cloud_shade(over);

    // Up the sun's light from the map, through the shell any cloud lies in.
    float depth = 0.0;
    float top = over.top;
    const float sun_mu = dot(ground, sun);
    const float2 out = sphere_hits(sun_mu, radius + laid_at, laid_at, push.bounds.y, radius);
    if (sun_mu > 0.0 && out.y > 0.0 && push.bounds.y > push.bounds.x) {
        const uint steps =
            clamp(uint(ceil(out.y / shadow_step)), least_shadow_steps, most_shadow_steps);
        const float step = out.y / float(steps);
        // Offset alike each frame, so the shadow holds still.
        const float jitter = interleaved(float2(id.xy));
        const float3 start = ground * (radius + laid_at);
        top = 0.0;
        for (uint at = 0; at < steps; ++at) {
            const float3 point = start + (sun * ((float(at) + jitter) * step));
            const float distance = length(point);
            const float point_height = distance - radius;
            const Cell cell = wandering_cell_at(point / distance, radius);
            float billows = 0.0;
            const float cloud = cloud_at(point, point_height, cell, texel, true, billows);
            if (cloud > 0.0) {
                depth += cloud * densest_in(cell) * step;
                top = max(top, cell.top);
            }
        }
        if (depth <= 0.0) {
            top = over.top;
        }
    }
    shadow_map[id.xy] = float4(exp(-depth), shade.x, shade.y, top);
}
