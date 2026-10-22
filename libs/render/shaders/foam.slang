import shore;
import waves;

// As hog::render::foam_size, foam_spacing, foam_lasting and foam_lasting_surf.
static const int foam_size = 512;
static const float foam_spacing = 1.0;
static const float foam_lasting = 3.0;
static const float foam_lasting_surf = 10.0;

// Of foam breaking white, as much again is left behind it as this, all told: as hog::waves'
// whitecaps and the surf split what's white, by how long each lasts.
static const float left_per_breaking = 3.0;
// Water shallower than this, metres, foams at the swash's edge, as ocean.slang has it; and how
// sharply the edge fades out onto the sand.
static const float swash_foam = 0.12;
static const float swash_rim = 0.02;
// Ground higher than this over the water, metres, takes no foam.
static const float dry_enough = 1.0;

struct Push {
    // xy how many texels the grid moved by, as hog::render::FoamStep's shift; z whether anything
    // is kept.
    int4 shift;
    // x how many seconds the step lasts, and y how long before now it's taken.
    float4 timing;
    // xyz how fast the water at the surface drifts, m/s, in the planet's frame.
    float4 drift;
};

[[vk::push_constant]] ConstantBuffer<Push> push;

[[vk::binding(0, 0)]] Sampler2D kept;
[[vk::binding(1, 0)]] [[vk::image_format("r32f")]] RWTexture2D<float> foam;
[[vk::binding(2, 0)]] ConstantBuffer<Waves> waves;
[[vk::binding(3, 0)]] Sampler2DArray shore_depths;
[[vk::binding(4, 0)]] Sampler2DArray shore_fetches;

// The point of sea level, from the eye, at `texel` on the grid: as hog::render::foam_texel finds
// texels, the other way.
float3 foam_rest(float2 texel) {
    const float3 east = waves.foam_east.xyz;
    const float3 north = waves.foam_north.xyz;
    const float2 along = float2(waves.foam_east.w, waves.foam_north.w) + (texel * foam_spacing);
    const float radius = waves.count_left_radius.z;
    const float across = dot(along, along);
    const float sagging = across / (radius + sqrt(max((radius * radius) - across, 0.0)));
    return waves.foam_anchor.xyz + (east * along.x) + (north * along.y) -
           (cross(east, north) * sagging);
}

// How much foam is left there, the step's share of it all told, as fast as it breaks there now;
// and how long it lasts there.
float2 breaking_at(float3 rest) {
    const float ago = push.timing.y;
    // Back then, the water here lay further back along its drift.
    const float3 then = rest - (push.drift.xyz * ago);
    float fetch;
    const float4 floor = floor_at(waves, shore_depths, shore_fetches, then, fetch, ago);
    const bool still = fetch > 0.0;
    const bool deep = floor.x >= waves.count_left_radius.w;
    if (floor.x < -dry_enough || (deep && !still && waves.foam.z <= 0.0)) {
        return float2(0.0, foam_lasting);
    }
    const WaveSurface water = wave_surface(waves, then, foam_spacing, floor.xyz, fetch, ago);
    // As ocean.slang has it, still water's surface kept out under the ground breaks on nothing.
    const bool kept_under = still && floor.x <= 0.0;
    const Surf surf = surf_at(waves, water, kept_under ? deep_floor() : floor.xyz);
    // The pattern drifts with the water, so it stands as it does now.
    const FoamPattern pattern = foam_pattern_at(
        waves, water, surf, coarse_foam_at(waves, rest, foam_spacing), rest, foam_spacing);
    const Foam now = foam_at(waves, water, surf, pattern, foam_spacing);
    // The swash's edge leaves its foam on the sand as it draws back.
    const float edge = (still ? 0.0 : 1.0) * saturate(floor.x / swash_rim) *
                       (1.0 - smoothstep(0.0, swash_foam, floor.x)) *
                       saturate(2.0 * (pattern.fine + pattern.across + 0.3));
    const float rate = left_per_breaking * (now.whitecaps + max(now.rollers, edge));
    const float surfing = surf.white / max(surf.white + water.white.z, 1e-6);
    return float2(rate, lerp(foam_lasting, foam_lasting_surf, surfing));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void compute_main(uint3 id: SV_DispatchThreadID) {
    const int2 from = int2(id.xy) + push.shift.xy;
    float left = 0.0;
    if (push.shift.z != 0 && all(from >= 0) && all(from < foam_size)) {
        left = kept.Load(int3(from, 0)).x;
    }
    const float2 breaking =
        waves.foam_anchor.w > 0.0 ? breaking_at(foam_rest(float2(id.xy) + 0.5))
                                  : float2(0.0, foam_lasting);
    const float fading = exp(-push.timing.x / breaking.y);
    foam[id.xy] = (left * fading) + ((1.0 - fading) * breaking.x);
}
