import shore;
import waves;

// As hog::render::ripple_size, ripple_spacing, ripple_edge, ripple_soaking and ripple_shallowest.
static const int ripple_size = 512;
static const float ripple_spacing = 0.125;
static const int ripple_edge = 48;
static const float ripple_soaking = 6.0;
static const float ripple_shallowest = 0.05;
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

// Along each row, the ripples as they lay before the step, where the grid now lies: height plus i
// times potential, none beyond the grid as it lay.
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
        line[reversed(at)] = ripple;
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

// Along each row, back from waves to the ripples they make; and what's kept of them.
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
        ripples[texel] = line[at] * scale * kept_at(texel);
    }
}
