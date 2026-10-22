import shore;
import waves;

// As hog::render::ripple_size, ripple_spacing, ripple_edge, ripple_soaking, ripple_shallowest,
// ripple_push and ripple_most_bodies.
static const int ripple_size = 512;
static const float ripple_spacing = 0.125;
static const int ripple_edge = 48;
static const float ripple_soaking = 6.0;
static const float ripple_shallowest = 0.05;
static const float ripple_push = 0.05;
static const int ripple_most_bodies = 16;
// How far past a body, metres, its push fades out, smoothly enough that it makes no ripples too
// short for the grid.
static const float push_fading = 0.25;
// The water's surface at a body is found as the sea draws it seen over so many metres.
static const float body_footprint = 0.25;
// How many times ripple_size halves to one, and the threads that step a row or column of it, a
// pair of texels each.
static const uint ripple_halvings = 9;
static const uint threads = ripple_size / 2;
static const float full_turn = 6.283185307;
// The water's kinematic viscosity, m²/s, which smooths the shortest ripples soonest.
static const float viscosity = 1e-6;

struct Push {
    // xy how many texels the grid moved by, as hog::render::GridStep's shift; z whether anything
    // is kept.
    int4 shift;
    // x how many seconds the step lasts.
    float4 timing;
};

[[vk::push_constant]] ConstantBuffer<Push> push;

// x how high the water stands, metres, and y the potential the water at its surface moves by,
// m²/s.
[[vk::binding(0, 0)]] [[vk::image_format("rg32f")]] RWTexture2D<float2> ripples;
// The ripples as waves, each texel a complex number, between steps along rows and columns.
[[vk::binding(1, 0)]] [[vk::image_format("rg32f")]] RWTexture2D<float2> spectrum;
[[vk::binding(2, 0)]] ConstantBuffer<Waves> waves;
[[vk::binding(3, 0)]] Sampler2DArray shore_depths;
[[vk::binding(4, 0)]] Sampler2DArray shore_fetches;

// As hog::render::RippleBody has them.
struct Bodies {
    // xy under the feet, texels; z metres they stand over sea level; w the radius.
    float4 feet[ripple_most_bodies];
    // xyz from the feet to the top, metres along the grid's rows, its columns and up.
    float4 tall[ripple_most_bodies];
    // x how many there are.
    int4 count;
};

[[vk::binding(5, 0)]] ConstantBuffer<Bodies> bodies;

// A row or column being transformed, or a pair of columns.
groupshared float2 line[2 * ripple_size];

float2 times(float2 a, float2 b) {
    return float2((a.x * b.x) - (a.y * b.y), (a.x * b.y) + (a.y * b.x));
}

float2 conjugate(float2 a) {
    return float2(a.x, -a.y);
}

// Where the texel at `at` along the line lies in it once its bits are reversed, as the transform
// takes them in.
uint reversed(uint at) {
    return reversebits(at) >> (32 - ripple_halvings);
}

// Transforms the line from `first` in place, from what each texel holds to how much of each wave
// it holds, or back if `back`, but for scaling down; its texels laid in bit-reversed order. Each
// thread takes `pair` of the transform's butterflies.
void transform(uint first, uint pair, bool back) {
    const float turning = back ? full_turn : -full_turn;
    for (uint span = 1; span < ripple_size; span *= 2) {
        GroupMemoryBarrierWithGroupSync();
        const uint within = pair % span;
        const uint low = first + ((pair / span) * 2 * span) + within;
        const uint high = low + span;
        float2 twiddle;
        sincos(turning * float(within) / float(2 * span), twiddle.y, twiddle.x);
        const float2 turned = times(line[high], twiddle);
        line[high] = line[low] - turned;
        line[low] = line[low] + turned;
    }
    GroupMemoryBarrierWithGroupSync();
}

// Of waves along the grid at `index`, counted as the transform counts them, how many radians each
// turns through a metre.
float wavenumber(uint index) {
    const int signed_index = index < ripple_size / 2 ? int(index) : int(index) - ripple_size;
    return full_turn * float(signed_index) / (float(ripple_size) * ripple_spacing);
}

// Over the step, the wave of `wavenumber` rad/m as `own` holds it, its height plus i times its
// potential, where the wave running the other way holds `opposite`: as deep water carries it, each
// part of its height turned into potential and back, as fast as its length has it.
float2 carried(float2 own, float2 opposite, float2 wavenumber) {
    const float2 height = (own + conjugate(opposite)) / 2.0;
    const float2 difference = own - conjugate(opposite);
    // That difference over 2i.
    const float2 potential = float2(difference.y, -difference.x) / 2.0;
    const float k = length(wavenumber);
    if (k <= 0.0) {
        return own;
    }
    const float omega = sqrt(gravity * k);
    float cosine;
    float sine;
    sincos(omega * push.timing.x, sine, cosine);
    const float smoothing = exp(-2.0 * viscosity * k * k * push.timing.x);
    const float2 height_after = ((height * cosine) + (potential * (k / omega) * sine)) * smoothing;
    const float2 potential_after =
        ((potential * cosine) - (height * (omega / k) * sine)) * smoothing;
    return height_after + float2(-potential_after.y, potential_after.x);
}

// Metres from `point` to the segment from `low` to `high`.
float from_segment(float3 point, float3 low, float3 high) {
    const float3 along = high - low;
    const float length_squared = dot(along, along);
    const float t = length_squared > 0.0 ? saturate(dot(point - low, along) / length_squared) : 0.0;
    return distance(point, low + (along * t));
}

float from_segment(float2 point, float2 low, float2 high) {
    return from_segment(float3(point, 0.0), float3(low, 0.0), float3(high, 0.0));
}

// Metres over sea level the water's surface stands at `texel` of the grid; sea level without one.
float waterline_at(float2 texel) {
    if (waves.ripple_anchor.w <= 0.0) {
        return 0.0;
    }
    const float3 rest =
        grid_rest(waves, waves.ripple_anchor, waves.ripple_east, waves.ripple_north, texel);
    float fetch;
    const float4 floor = floor_at(waves, shore_depths, shore_fetches, rest, fetch);
    return water_height_over(waves, rest, body_footprint, floor, fetch);
}

// How far the bodies crossing the surface push the water down at `texel`, metres: as far as
// ripple_push where they cross it, fading out round them.
float pushed_at(int2 texel) {
    const float2 point = (float2(texel) + 0.5) * ripple_spacing;
    float pushed = 0.0;
    for (int i = 0; i < min(bodies.count.x, ripple_most_bodies); ++i) {
        const float4 feet = bodies.feet[i];
        const float3 tall = bodies.tall[i].xyz;
        const float radius = feet.w;
        const float reach = radius + push_fading;
        // Between the middles of its ends.
        const float3 rising = tall * (radius / max(length(tall), 1e-6));
        const float3 low = float3(feet.xy * ripple_spacing, feet.z) + rising;
        const float3 high = float3(feet.xy * ripple_spacing, feet.z) + tall - rising;
        if (from_segment(point, low.xy, high.xy) >= reach) {
            continue;
        }
        const float off = from_segment(float3(point, waterline_at(feet.xy)), low, high);
        const float across = saturate(off / reach);
        const float pressing = 1.0 - (across * across);
        pushed = max(pushed, ripple_push * pressing * pressing);
    }
    return pushed;
}

// Along each row, the ripples as they lay before the step, where the grid now lies: height plus i
// times potential, none beyond the grid as it lay; pushed on over the step as though the water
// stood as much higher as the bodies push it down, and so pushed back up by it.
[shader("compute")]
[numthreads(threads, 1, 1)]
void rows_main(uint3 thread: SV_GroupThreadID, uint3 group: SV_GroupID) {
    const int row = int(group.x);
    for (uint at = thread.x; at < ripple_size; at += threads) {
        const int2 from = int2(int(at), row) + push.shift.xy;
        float2 ripple = float2(0.0);
        if (push.shift.z != 0 && all(from >= 0) && all(from < ripple_size)) {
            ripple = ripples[from];
        }
        line[reversed(at)] = ripple + float2(pushed_at(int2(int(at), row)), 0.0);
    }
    transform(0, thread.x, false);
    for (uint at = thread.x; at < ripple_size; at += threads) {
        spectrum[int2(int(at), row)] = line[at];
    }
}

// Down each column and the column of waves running the other way along the rows, each wave carried
// over the step and back down them.
[shader("compute")]
[numthreads(threads, 1, 1)]
void columns_main(uint3 thread: SV_GroupThreadID, uint3 group: SV_GroupID) {
    const uint column = group.x;
    const uint opposite = (ripple_size - column) % ripple_size;
    for (uint at = thread.x; at < ripple_size; at += threads) {
        line[reversed(at)] = spectrum[int2(int(column), int(at))];
        line[ripple_size + reversed(at)] = spectrum[int2(int(opposite), int(at))];
    }
    transform(0, thread.x, false);
    transform(ripple_size, thread.x, false);
    float2 own[2];
    float2 others[2];
    for (uint i = 0; i < 2; ++i) {
        const uint at = thread.x + (i * threads);
        const uint back = (ripple_size - at) % ripple_size;
        own[i] = carried(line[at], line[ripple_size + back],
                         float2(wavenumber(column), wavenumber(at)));
        others[i] = carried(line[ripple_size + at], line[back],
                            float2(wavenumber(opposite), wavenumber(at)));
    }
    GroupMemoryBarrierWithGroupSync();
    for (uint i = 0; i < 2; ++i) {
        const uint at = thread.x + (i * threads);
        line[reversed(at)] = own[i];
        line[ripple_size + reversed(at)] = others[i];
    }
    transform(0, thread.x, true);
    transform(ripple_size, thread.x, true);
    for (uint at = thread.x; at < ripple_size; at += threads) {
        spectrum[int2(int(column), int(at))] = line[at];
        spectrum[int2(int(opposite), int(at))] = line[ripple_size + at];
    }
}

// How much of a ripple at `texel` is kept over a step: less towards the grid's edge, where they're
// soaked up, and none on ground or water too shallow to ripple.
float kept_at(int2 texel) {
    const int2 inside = min(texel, int2(ripple_size - 1) - texel);
    const float edge = saturate(1.0 - ((float(min(inside.x, inside.y)) + 0.5) / ripple_edge));
    float kept = exp(-ripple_soaking * edge * edge * push.timing.x);
    if (waves.ripple_anchor.w > 0.0) {
        const float3 rest = grid_rest(waves, waves.ripple_anchor, waves.ripple_east,
                                      waves.ripple_north, float2(texel) + 0.5);
        float fetch;
        const float4 floor = floor_at(waves, shore_depths, shore_fetches, rest, fetch);
        kept *= saturate(floor.x / ripple_shallowest);
    }
    return kept;
}

// Along each row, back from waves to the ripples they make, as high as the water stands; and
// what's kept of them.
[shader("compute")]
[numthreads(threads, 1, 1)]
void settle_main(uint3 thread: SV_GroupThreadID, uint3 group: SV_GroupID) {
    const int row = int(group.x);
    for (uint at = thread.x; at < ripple_size; at += threads) {
        line[reversed(at)] = spectrum[int2(int(at), row)];
    }
    transform(0, thread.x, true);
    const float scale = 1.0 / float(ripple_size * ripple_size);
    for (uint at = thread.x; at < ripple_size; at += threads) {
        const int2 texel = int2(int(at), row);
        const float2 ripple = (line[at] * scale) - float2(pushed_at(texel), 0.0);
        ripples[texel] = ripple * kept_at(texel);
    }
}
