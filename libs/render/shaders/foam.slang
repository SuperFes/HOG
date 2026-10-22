import shore;
import waves;

// As hog::render::foam_spacing, foam_lasting, foam_lasting_surf and foam_lasting_stranded.
static const float foam_spacing = 1.0;
static const float foam_lasting = 3.0;
static const float foam_lasting_surf = 10.0;
static const float foam_lasting_stranded = 20.0;

// Of foam breaking white, as much again is left behind it as this, all told: as hog::waves'
// whitecaps and the surf split what's white, by how long each lasts.
static const float left_per_breaking = 3.0;
// Water shallower than this, metres, foams at the swash's edge, as ocean.slang has it; and how
// sharply the edge fades out onto the sand.
static const float swash_foam = 0.12;
static const float swash_rim = 0.02;
// Ground higher than this over the water, metres, takes no foam.
static const float dry_enough = 1.0;
// Of the foam on water drawing back off the ground, the share left stranded there; the rest the
// backwash carries off.
static const float stranding = 0.3;

struct Push {
    // xy how many texels the grid moved by, as hog::render::GridStep's shift; z whether anything
    // is kept.
    int4 shift;
    // x how many seconds the step lasts, and y how long before now it's taken.
    float4 timing;
    // xyz how fast the water at the surface drifts, m/s, in the planet's frame.
    float4 drift;
};

[[vk::push_constant]] ConstantBuffer<Push> push;

[[vk::binding(0, 0)]] Sampler2D kept;
// x the foam left, all told; y how much of what it would have been left by is known, as the grid
// saw it; z the foam stranded on the ground.
[[vk::binding(1, 0)]] [[vk::image_format("rgba32f")]] RWTexture2D<float4> foam;
[[vk::binding(2, 0)]] ConstantBuffer<Waves> waves;
[[vk::binding(3, 0)]] Sampler2DArray shore_depths;
[[vk::binding(4, 0)]] Sampler2DArray shore_fetches;

// The point of sea level, from the eye, at `texel` on the grid.
float3 foam_rest(float2 texel) {
    return grid_rest(waves, waves.foam_anchor, waves.foam_east, waves.foam_north, texel);
}

// How much foam is left there, the step's share of it all told, as fast as it breaks there now;
// how long it lasts there; and 1 if the water's drawn back off the ground there.
float3 breaking_at(float3 rest) {
    const float ago = push.timing.y;
    // Back then, the water here lay further back along its drift.
    const float3 then = rest - (push.drift.xyz * ago);
    float fetch;
    const float4 floor = floor_at(waves, shore_depths, shore_fetches, then, fetch, ago);
    const bool still = fetch > 0.0;
    const bool deep = floor.x >= waves.count_left_radius.w;
    const float dry = floor.x <= 0.0 ? 1.0 : 0.0;
    if (floor.x < -dry_enough || (deep && !still && waves.foam.z <= 0.0)) {
        return float3(0.0, foam_lasting, dry);
    }
    const WaveSurface water = wave_surface(waves, then, foam_spacing, floor.xyz, fetch, ago);
    // As ocean.slang has it, still water's surface kept out under the ground breaks on nothing.
    const bool kept_under = still && floor.x <= 0.0;
    const Surf surf = surf_at(waves, water, kept_under ? deep_floor() : floor.xyz);
    // The pattern drifts with the water, so it stands as it does now.
    const FoamPattern pattern = foam_pattern_at(
        waves, water, surf, coarse_foam_at(waves, rest, foam_spacing), rest, foam_spacing);
    const Foam now = foam_at(waves, water, surf, pattern, foam_spacing);
    // The swash's edge leaves its foam on the sand as it draws back, as ocean.slang foams it: as
    // much as the waves running up are high.
    const float edge = (still ? 0.0 : 1.0) * saturate(running_up(waves) / swash_foam) *
                       saturate(floor.x / swash_rim) *
                       (1.0 - smoothstep(0.0, swash_foam, floor.x)) *
                       saturate(2.0 * (pattern.fine + pattern.across + 0.3));
    const float rate = left_per_breaking * (now.whitecaps + max(now.rollers, edge));
    const float surfing = surf.white / max(surf.white + water.white.z, 1e-6);
    return float3(rate * (1.0 - dry), lerp(foam_lasting, foam_lasting_surf, surfing), dry);
}

// What was stranded on the ground at `from`, texels on the grid as it lay a step before, between
// texels as the water's drifted off them; none beyond its edges. Taken as filtering, but finer
// than samplers part texels, as the water drifts a little each step.
float stranded_from(float2 from) {
    const int2 low = int2(floor(from));
    const float2 part = from - float2(low);
    float stranded = 0.0;
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 2; ++x) {
            const int2 at = low + int2(x, y);
            if (all(at >= 0) && all(at < foam_size)) {
                const float2 share = lerp(1.0 - part, part, float2(x, y));
                stranded += kept.Load(int3(at, 0)).z * share.x * share.y;
            }
        }
    }
    return stranded;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void compute_main(uint3 id: SV_DispatchThreadID) {
    const int2 from = int2(id.xy) + push.shift.xy;
    float2 left = float2(0.0);
    float stranded = 0.0;
    if (push.shift.z != 0) {
        if (all(from >= 0) && all(from < foam_size)) {
            left = kept.Load(int3(from, 0)).xy;
        }
        // What's stranded stays with the ground, which the grid drifted off with the water.
        const float2 drifted =
            float2(dot(push.drift.xyz, waves.foam_east.xyz),
                   dot(push.drift.xyz, waves.foam_north.xyz)) *
            waves.foam_anchor.w * push.timing.x;
        stranded = stranded_from(float2(from) + drifted);
    }
    const float3 breaking =
        waves.foam_anchor.w > 0.0 ? breaking_at(foam_rest(float2(id.xy) + 0.5))
                                  : float3(0.0, foam_lasting, 0.0);
    // The water strands some of what's on it as it draws back off the ground, and takes it back up
    // as it runs over it again.
    if (breaking.z > 0.0) {
        stranded += stranding * left.x;
        left.x = 0.0;
    } else {
        left.x += stranded;
        stranded = 0.0;
    }
    const float fading = exp(-push.timing.x / breaking.y);
    foam[id.xy] = float4((left * fading) + ((1.0 - fading) * float2(breaking.x, 1.0)),
                         stranded * exp(-push.timing.x / foam_lasting_stranded), 0.0);
}
