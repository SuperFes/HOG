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
// m/s the cloud drifts over the ground, in the planet's frame.
[[vk::binding(15, 0)]] SamplerCube drift_before;
[[vk::binding(16, 0)]] SamplerCube drift_after;
// The cloud in each of the air's layers, bottom up, as client::LayerTexel: its cover, and the root
// of a sixteenth of its water's millimetres. Lighting's air_layers says how the layers lie.
[[vk::binding(17, 0)]] SamplerCubeArray layers_before;
[[vk::binding(18, 0)]] SamplerCubeArray layers_after;
// As render::shape_noise and render::detail_noise.
[[vk::binding(9, 0)]] Sampler3D shape_noise;
[[vk::binding(10, 0)]] Sampler3D detail_noise;
// Premultiplied, before tone mapping; and the metres to the cloud, weighed by what it hides.
[[vk::binding(11, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> marched;
[[vk::binding(12, 0)]] [[vk::image_format("rg32f")]] RWTexture2D<float2> distances;
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
    // xyz metres the noise is shifted by besides its drift, in the planet's frame and within the
    // clouds' period; w how
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
// Of the light through the cloud, the share past which it's taken as hidden.
static const float least_seen_through = 0.01;
// Of the golden ratio, the part past 1: frame after frame, it spreads offsets evenly.
static const float golden = 0.618034;
static const float shortest_step = 40.0;
static const float longest_step = 2000.0;
// Metres along the view clouds are marched over at most.
static const float march_reach = 100000.0;
// Metres a march goes through cloud before it reads afresh the column of weather that lights it
// from above, which changes far more slowly than the cloud's billows.
static const float lit_afresh = 300.0;
// Where the light reaching each step is found, towards the sun, in metres.
static const uint light_steps = 6;
static const float light_reach = 2000.0;
// Scattered forwards strongly, and a little back; the droplets scatter on, not absorb.
static const float forward = 0.8;
static const float backward = -0.3;
static const float backward_share = 0.25;
static const float cloud_albedo = 0.99;
// The weather's looked up a little off from where it's wanted, by a broad noise, so the edges of
// what the cells hold wander, not run straight across them. A whole number of tiles fits in
// clouds.hpp's cloud_period.
static const float wander_tile = 32000.0;
static const float wander = 8000.0;

// The noise's sizes. Billows wider than the weather's cells leave each cell's cloud a flat slab
// cut off straight at its edges, so on a planet small enough that its cells are narrower than two
// billow tiles, all of them halve until they fit; halving keeps a whole number in the period.
struct Scales {
    float shape;
    float detail;
    float wander_tile;
    float wander;
};

Scales scales() {
    uint across;
    uint down;
    weather_before.GetDimensions(across, down);
    const float cell = push.radii.x * 1.5707963 / float(across);
    const float halvings = clamp(ceil(log2(shape_tile / (2.0 * cell))), 0.0, 7.0);
    const float shrunk = exp2(-halvings);
    Scales made;
    made.shape = shape_tile * shrunk;
    made.detail = detail_tile * shrunk;
    made.wander_tile = wander_tile * shrunk;
    made.wander = wander * shrunk;
    return made;
}
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
// Rain falls from wherever the cloud billows over it, a quarter, a half and three quarters of the way
// up, as much of the sky as its whole column covers, and from within its edges: where its density,
// as a share of its cover, is past the least, fully past the most. It thickens over this many
// metres below the base, so the shafts have no flat tops.
static const uint rain_from = 3;
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
// Metres along a view over which cloud thins away to nothing where it meets what's drawn, so cloud
// lying on the ground or against a mountain fades into it rather than being cut off by it.
static const float meeting_fade = 40.0;

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

// Where the cloud over a direction now lay when the maps before and after were taken, by the drift
// there, so wherever it's seen from it's the same cloud. The cloud blows on with the wind; the
// air's damp keeps to the air's slower pace. Each weather drifts on from when it came, so a new
// one's coming moves nothing.
struct Blown {
    float3 before;
    float3 after;
};

Blown blown_from(float3 direction) {
    Blown blown;
    blown.before = normalize((direction * lighting.weather.x) -
                             (drift_before.SampleLevel(direction, 0.0).xyz * lighting.weather_age.x));
    blown.after = normalize((direction * lighting.weather.x) -
                            (drift_after.SampleLevel(direction, 0.0).xyz * lighting.weather_age.y));
    return blown;
}

Cell cell_at(float3 direction, Blown from) {
    const float3 from_before = from.before;
    const float3 from_after = from.after;
    const float4 weather = lerp(weather_before.SampleLevel(from_before, 0.0),
                                weather_after.SampleLevel(from_after, 0.0), push.radii.z);
    const SkyCell blown = sky_cell(lerp(sky_before.SampleLevel(from_before, 0.0),
                                        sky_after.SampleLevel(from_after, 0.0), push.radii.z));
    const SkyCell sky = sky_cell(lerp(sky_before.SampleLevel(direction, 0.0),
                                      sky_after.SampleLevel(direction, 0.0), push.radii.z));
    Cell cell;
    cell.cover = weather.r;
    cell.mm = weather.g * weather.g * 16.0;
    cell.base = blown.base;
    cell.top = max(blown.top, blown.base);
    cell.falling = weather.b * weather.b * 256.0;
    cell.snow = weather.a;
    return cell;
}

// Seconds a billow flows before it's faded for the next: clouds.hpp's flow_period.
static const float flow_period = 240.0;

// Where the noise lies for billows at `at`: shifted by the drift there, whoever looks, in two
// phases faded one into the other so the drift's shear never winds them up.
struct Flow {
    float3 first;
    float3 second;
    // The share of the first.
    float share;
};

Flow flow_at(float3 at) {
    const float3 direction = normalize(at);
    const float3 drift = lerp(drift_before.SampleLevel(direction, 0.0).xyz,
                              drift_after.SampleLevel(direction, 0.0).xyz, push.radii.z);
    const float phase = lighting.weather_age.z / flow_period;
    const float other = frac(phase + 0.5);
    Flow flow;
    flow.first = push.moved.xyz + (drift * ((phase - 0.5) * flow_period));
    flow.second = push.moved.xyz + (drift * ((other - 0.5) * flow_period));
    flow.share = 1.0 - abs((2.0 * phase) - 1.0);
    return flow;
}

// `noise` at `at`, `tile` metres across its volume, as the billows have flowed.
float4 flowed(Sampler3D noise, float3 at, Flow flow, float tile, float level) {
    return lerp(noise.SampleLevel((at - flow.second) / tile, level),
                noise.SampleLevel((at - flow.first) / tile, level), flow.share);
}

// Where the weather's read for `direction` on a planet of `radius`, its edges wandering, and where
// the cloud there was blown from.
struct Place {
    float3 direction;
    Blown from;
};

Place wandered(float3 direction, float radius) {
    const Scales sizes = scales();
    const float3 ground = direction * radius;
    const float4 broad = flowed(shape_noise, ground, flow_at(ground), sizes.wander_tile, 1.0);
    Place place;
    place.direction = normalize(ground + ((broad.gba - 0.5) * sizes.wander));
    place.from = blown_from(place.direction);
    return place;
}

Cell wandering_cell_at(float3 direction, float radius) {
    const Place place = wandered(direction, radius);
    return cell_at(place.direction, place.from);
}

// The cloud at a height in the air's layers: how much of the sky it covers, and per metre how much
// light it takes where it's as thick as it gets.
struct Layered {
    float cover;
    float density;
};

// Of the layer `index`, its cover and density at the place.
Layered layer_at(Place place, float index) {
    const float2 texel = lerp(layers_before.SampleLevel(float4(place.from.before, index), 0.0).rg,
                              layers_after.SampleLevel(float4(place.from.after, index), 0.0).rg,
                              push.radii.z);
    const float4 air_layers = lighting.air_layers;
    const float depth = air_layers.z * pow(air_layers.w, index);
    const float water = texel.g * texel.g * 16.0;
    Layered layer;
    layer.cover = texel.r;
    // The billows fill about half of what it covers, so where they're thickest they're twice as
    // dense as its water spread through it all.
    layer.density = min(2.0 * depth_per_mm * water / (max(texel.r, least_cover) * depth), densest);
    return layer;
}

// The cloud `height` metres above sea level at the place: the layers' about it, blended from each
// one's middle to the next, so the cloud runs on smoothly up through them and across.
Layered layered_at(Place place, float height) {
    const float4 air_layers = lighting.air_layers;
    const float count = air_layers.x;
    Layered none;
    none.cover = 0.0;
    none.density = 0.0;
    if (count < 1.0) {
        return none;
    }
    // How many layers up from the bottom, as weather::Grid lays them.
    const float over = max(height - air_layers.y, 0.0);
    const float ratio = air_layers.w;
    const float up = ratio > 1.0001 ? log2(1.0 + (over * (ratio - 1.0) / air_layers.z)) / log2(ratio)
                                    : over / air_layers.z;
    if (up >= count) {
        return none;
    }
    const float between = clamp(up - 0.5, 0.0, count - 1.0);
    const float lower = floor(between);
    const Layered below = layer_at(place, lower);
    const Layered above = layer_at(place, min(lower + 1.0, count - 1.0));
    Layered layered;
    layered.cover = lerp(below.cover, above.cover, between - lower);
    layered.density = lerp(below.density, above.density, between - lower);
    return layered;
}

float remap(float value, float from_low, float from_high, float to_low, float to_high) {
    return to_low + ((value - from_low) * (to_high - to_low) / (from_high - from_low));
}

// How much of the cloud `here` there is, from none to 1, `at` metres in the planet's frame; with its
// edges worn finely when `worn`. `footprint` is the metres a pixel spans there. `billows` is left
// as how much the cloud billows there, from 0 to 1 evenly.
float cloud_at(float3 at, Layered here, float footprint, bool worn, out float billows) {
    billows = 0.0;
    if (here.cover < 1.0 / 255.0) {
        return 0.0;
    }
    const Scales sizes = scales();
    const Flow flow = flow_at(at);
    const float4 shape = flowed(shape_noise, at, flow, sizes.shape,
                                max(log2(footprint * shape_texels / sizes.shape), 0.0));
    billows = shape.r;
    // The billows are spread evenly, so about as much of the sky's covered as the cover says, and a
    // full cover leaves no gaps. Each layer covering less than the one under it, broken cloud
    // billows up into domes.
    float cloud = saturate(remap(shape.r, 1.0 - (here.cover * overfill), 1.0, 0.0, 1.0));
    // Broken cloud's worn by the cells, where it's thin.
    const float wearing = (shape.g * 0.625) + (shape.b * 0.25) + (shape.a * 0.125);
    cloud = saturate(remap(cloud, (1.0 - wearing) * 0.25 * (1.0 - here.cover), 1.0, 0.0, 1.0));
    if (worn && cloud > 0.0) {
        const float detail = flowed(detail_noise, at, flow, sizes.detail,
                                    max(log2(footprint * detail_texels / sizes.detail), 0.0))
                                 .a;
        // Billowing; less so the more of the sky's covered, so a full cover keeps no gaps.
        cloud = saturate(
            remap(cloud, (1.0 - detail) * 0.3 * (1.0 - (0.8 * here.cover)), 1.0, 0.0, 1.0));
    }
    return cloud;
}

float thickness_of(Cell cell) {
    return max(cell.top - cell.base, 1.0);
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
    const float3 at = direction * (push.radii.x + (height / streak_stretch));
    const float detail = flowed(detail_noise, at, flow_at(at), streak_tile,
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
    Layered column;
    column.cover = cell.cover;
    column.density = 0.0;
    float cloud = 0.0;
    for (uint at = 1; at <= rain_from; ++at) {
        const float height = lerp(cell.base, cell.top, float(at) / float(rain_from + 1));
        float unused = 0.0;
        cloud = max(cloud, cloud_at(direction * (push.radii.x + height), column,
                                    max(footprint, rain_footprint), false, unused));
    }
    return saturate(remap(cloud, rain_least, rain_most, 0.0, 1.0));
}

// The view's way through the sky, and the picture's texel it's for.
struct Ray {
    float3 view;
    float2 uv;
    // How far it reaches: to what's drawn, else to the ground; and to what's drawn alone.
    float reach;
    float drawn;
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

// The cloud along `ray`, from `start` to `end`, fading into what's drawn where it meets it,
// premultiplied, before tone mapping; `distance` is left as the metres to it, weighed by what it
// hides.
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
    // The column lighting the cloud from above, as last read and where; read before it's first
    // used, as the first step through cloud always reads it.
    Cell lit_cell = (Cell)0;
    float lit_at_along = -1e30;
    for (uint at = 0; at < most_steps && along < end && seen_through > least_seen_through; ++at) {
        const float coarse = clamp(along * step_growth, shortest_step, longest);
        const float3 from_eye = view * along;
        const float point_height = drawn_height(from_eye, up, eye_height, faked);
        const float3 direction = on_planet(from_eye, up, centre_to_eye, faked, radius);
        const Place place = wandered(direction, radius);
        const Layered here = layered_at(place, point_height);
        const float3 point = direction * (radius + point_height);
        const float footprint = along * pixel;
        float billows = 0.0;
        const float extinction = cloud_at(point, here, footprint, true, billows) * here.density *
                                 saturate((ray.drawn - along) / meeting_fade);
        const bool within = extinction > 0.0;
        const float fine = clamp(step_depth / max(extinction, 1e-6),
                                 max(footprint, coarse / most_refined), coarse);
        if (within && was_clear && along - clear_at > fine * 1.01) {
            // Stepped into cloud: back to where it was clear, and on in finer steps.
            stepping_to = along;
            refined = fine;
            along = clear_at + (refined * ray.jitter);
            continue;
        }
        const float step = within ? fine : (along < stepping_to ? refined : coarse);
        if (within) {
            const float3 point_up = normalize((up * centre_to_eye) + from_eye);
            const float sun_mu = dot(point_up, sun);
            if (abs(along - lit_at_along) > lit_afresh) {
                lit_cell = cell_at(place.direction, place.from);
                lit_at_along = along;
            }
            // What the cloud towards the sun takes, as if the weather there were the same: its layers
            // read at every other step's middle, from here, and blended for the steps between.
            float towards_sun = 0.0;
            float before = 0.0;
            float unused = 0.0;
            Layered lower = here;
            float lower_middle = 0.0;
            Layered upper = here;
            float upper_middle = 0.0;
            for (uint light = 0; light < light_steps; ++light) {
                const float share = float(light + 1) / float(light_steps);
                const float after = light_reach * share * share;
                const float middle = (before + after) * 0.5;
                const float3 lit_at = point + (sun * middle);
                if (light % 2 == 0) {
                    const float next_share = float(light + 2) / float(light_steps);
                    const float next_middle =
                        0.5 * (after + (light_reach * next_share * next_share));
                    lower = upper;
                    lower_middle = upper_middle;
                    upper = layered_at(place, point_height + (next_middle * sun_mu));
                    upper_middle = next_middle;
                }
                const float blend = saturate((middle - lower_middle) / (upper_middle - lower_middle));
                Layered lit;
                lit.cover = lerp(lower.cover, upper.cover, blend);
                lit.density = lerp(lower.density, upper.density, blend);
                towards_sun +=
                    cloud_at(lit_at, lit, footprint, false, unused) * lit.density * (after - before);
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
            const float column = depth_per_mm * lit_cell.mm / max(lit_cell.cover, least_cover);
            const float over = column *
                               saturate((lit_cell.top - point_height) / thickness_of(lit_cell)) *
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
        was_clear = !within;
        if (was_clear) {
            clear_at = along;
        }
        along += step;
    }
    // What's past where it's all but hidden is hidden too, as the sun is behind thick cloud; what's
    // seen of it is more of the same.
    if (seen_through <= least_seen_through) {
        radiance /= 1.0 - seen_through;
        weighed /= 1.0 - seen_through;
        seen_through = 0.0;
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
    ray.reach = to_ground(mu, centre_to_eye, eye_height, faked);
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
    float drawn_reach = 1e30;
    if (furthest > sky_depth) {
        const float4 drawn = mul(push.eye_from_clip, float4(clip, furthest, 1.0));
        drawn_reach = length(drawn.xyz / drawn.w);
        ray.reach = min(ray.reach, drawn_reach);
    }
    ray.drawn = drawn_reach;

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
    const float distance = seen.a > 0.0 ? ((nearer.a * nearer_distance) +
                                           (further.a * (1.0 - nearer.a) * further_distance)) /
                                              seen.a
                                        : 0.0;
    distances[id.xy] = float2(distance, drawn_reach);
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
            const Place place = wandered(point / distance, radius);
            const Layered here = layered_at(place, point_height);
            float billows = 0.0;
            const float cloud = cloud_at(point, here, texel, true, billows);
            if (cloud > 0.0) {
                depth += cloud * here.density * step;
                top = max(top, point_height);
            }
        }
        if (depth <= 0.0) {
            top = over.top;
        }
    }
    shadow_map[id.xy] = float4(exp(-depth), shade.x, shade.y, top);
}
