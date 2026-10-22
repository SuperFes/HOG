module waves;

import shore;

// As hog::render::drawn_waves: the most waves the sea is drawn with; and foam_waves, those of its
// foam's pattern.
public static const int drawn_waves = 48;
public static const int foam_waves = 26;
// Of those, the longer, which place the foam's patches; the rest make its lace.
static const int coarse_foam = 12;
// As hog::waves::choppiness: how much of its orbit's width the water moves along a wave as well as
// up and down.
static const float choppiness = 1.0;

// Waves fade as their length falls from this many footprints to half as many.
static const float footprints_shown = 4.0;

// As hog::waves' deep_enough, the shore's profile and breaking_depth in waves/shoaling.hpp.
static const float deep_enough = 3.14159265358979;
static const float dean_scale = 0.12;
static const float shelf_run = 100.0;
static const float shelf_depth = shelf_run * shelf_run * dean_scale * dean_scale * dean_scale;
static const float shallowest = 0.01;
static const float full_turn = 6.283185307;
static const float breaking_depth = 0.42;

// Each as it runs round the eye: over the few kilometres seen, the planet's up turns too little to
// change how high a wave shows or which way it runs along the ground.
public struct Wave {
    // xyz unit, in the planet's frame; w the wavenumber, rad/m.
    public float4 heading_wavenumber;
    // x metres high, y its phase through the eye, z the mean square slope of it and every shorter
    // wave, w the mean square of how high they rise.
    public float4 amplitude_phase_slope_heave;
    // xy which way it runs along the ground, in the eye's frame; z its wavenumber along it; w how
    // much it takes part in breaking white.
    public float4 along;
    // x the mean square slope of it and every shorter wave, each by the square of how much it
    // takes part in breaking white; yzw its lag terms, as hog::waves::lag_terms has them.
    public float4 breaking;
};

// Of the foam's pattern: xyz its wavenumber in the planet's frame, rad/m, w its phase through the
// eye; x its amplitude, y its length along the ground and z its wavenumber along it.
public struct FoamWave {
    public float4 wavenumber_phase;
    public float4 amplitude_length;
};

// Longest first.
public struct Waves {
    // x how many, y the mean square slope of the waves left out, z the planet's radius, w how deep
    // the water must be for none of them to feel the floor.
    public float4 count_left_radius;
    // xyz up through the eye, w its height above sea level.
    public float4 up_height;
    // The eye's frame along the ground: any two ways across, making a right-handed frame with up.
    public float4 first;
    public float4 second;
    // x how far the bunching of the waves that break must rise for them to break white, y its mean
    // square of the waves left out; z the share of the sea breaking white, w the share white with
    // what's left of it.
    public float4 foam;
    // Of the foam left: x how far over its mean the pattern's coarse field lies in its patches, and
    // y how near its fine field's mean the lace in them lies, both in standard deviations; z the
    // standard deviation of the bunching of the waves that break, and w of how whitecaps churn.
    public float4 foam_left;
    // As shore_depth takes them: where the depths round the eye lie, and how finely.
    public float4 shore_anchor;
    public float4 shore_east;
    public float4 shore_north;
    // xyz how the tide rises per metre from the eye, w how high it stands over the sea's mean at
    // the eye.
    public float4 tide;
    // Of the waves running up the shore, as hog::waves::run_up takes them: x the root of their
    // height times their wavelength in deep water, y how deep the water is where they begin to
    // set the sea up, and z and w how fast the swash of the waves and of their groups' surges
    // turn, rad/s. Then each's turning through the eye, as its wavenumber along the ground in xyz
    // and its phase in w.
    public float4 run_up;
    public float4 swash_incident;
    public float4 swash_surging;
    public Wave waves[drawn_waves];
    public FoamWave foam_pattern[foam_waves];
};

// As hog::waves' shoaling: the shallow wavenumber times the depth, at κ, as Hunt fit it.
float depth_wavenumber(float kappa) {
    const float sum =
        kappa *
        (0.6666666667 +
         (kappa *
          (0.3555555556 +
           (kappa *
            (0.1608465608 +
             (kappa * (0.0632098765 + (kappa * (0.0217540484 + (kappa * 0.0065407983))))))))));
    return sqrt((kappa * kappa) + (kappa / (1.0 + sum)));
}

// As hog::waves' shelf_lag and shoreface_lag, and their values and slopes against κ.
static const float shelf_lag[9] = { 1.2347241954,  -3.5420030563, 3.0825815818,
                                    -0.1219981919, -2.1095339280, 4.6660317033,
                                    -6.1531146267, 3.8080381810,  -0.8647258586 };
static const float shoreface_lag[9] = { 0.5626467669,  0.0,           -3.1638013226,
                                        3.9954707819,  -2.2227549352, 3.3481431461,
                                        -4.2511585803, 1.9068094313,  -0.1753552881 };

// A wave's phase lag at κ and its slope against κ, from its lag terms as hog::waves::lag_terms
// has them: x the turn, y the scale and z the offset.
float2 lag_at(float3 terms, float kappa) {
    const float t = sqrt(kappa / deep_enough);
    float value = 0.0;
    float slope = 0.0;
    if (kappa >= terms.x) {
        for (int n = 8; n >= 0; --n) {
            slope = (slope * t) + value;
            value = (value * t) + shelf_lag[n];
        }
        return shelf_run * float2(value, slope / (2.0 * deep_enough * t));
    }
    for (int n = 8; n >= 2; --n) {
        value = (value * t) + shoreface_lag[n];
        slope = (slope * t) + (float(n) * shoreface_lag[n]);
    }
    value = (value * t * t) + shoreface_lag[0];
    return float2(terms.z, 0.0) + (terms.y * float2(value, slope / (2.0 * deep_enough)));
}

// Metres over which the floor's slope is taken: half the finest depths' spacing, so it changes
// smoothly across them.
static const float floor_step = 4.0;

float depth_at(Waves waves, Sampler2DArray depths, float3 point) {
    return shore_depth(depths, waves.shore_anchor, waves.shore_east, waves.shore_north,
                       waves.count_left_radius.z, point);
}

// The sea's floor under `point`, from the eye, as shore_depth finds it in `depths`: x how deep the
// sea is over it at its mean, and yz how fast it deepens along the eye's first and second ways
// across, metres per metre; none at all where it's too deep for any wave to feel.
public float3 shore_floor(Waves waves, Sampler2DArray depths, float3 point) {
    const float depth = depth_at(waves, depths, point);
    if (depth >= waves.count_left_radius.w + shore_reach || sea_reached(depth) <= 0.0) {
        return float3(depth, 0.0, 0.0);
    }
    const float3 first = waves.first.xyz * floor_step;
    const float3 second = waves.second.xyz * floor_step;
    return float3(depth,
                  (depth_at(waves, depths, point + first) - depth_at(waves, depths, point - first)) /
                      (2.0 * floor_step),
                  (depth_at(waves, depths, point + second) -
                   depth_at(waves, depths, point - second)) /
                      (2.0 * floor_step));
}

// As hog::waves::run_up takes them: the slopes beaches are taken to lie within.
static const float gentlest_beach = 0.01;
static const float steepest_beach = 0.2;

// How high the sea stands over its mean at a point: `mean`, as the tide and the waves' set-up
// raise it, and the swash about that, each part as high as `swash` and turned through its period
// as the cosine and sine in `turning`.
public struct ShoreLevel {
    public float mean;
    public float2 swash;
    public float4 turning;
};

// At `point`, from the eye, over `floor` as shore_floor finds it: the tide, and the waves' set-up
// and swash as hog::waves::run_up has them, each wherever the sea reaches. The swash runs up
// beaches and over the shallows, and the set-up rises across the surf.
public ShoreLevel shore_level_at(Waves waves, float3 floor, float3 point) {
    ShoreLevel level;
    const float reached = sea_reached(floor.x);
    const float tide = (waves.tide.w + dot(waves.tide.xyz, point)) * reached;
    const float scale = waves.run_up.x * reached;
    const float beach = clamp(length(floor.yz), gentlest_beach, steepest_beach);
    const float under = floor.x + tide;
    const float setting = saturate(1.0 - (under / max(waves.run_up.y, 1e-3)));
    level.mean = tide + (setting * 0.35 * beach * scale);
    // Half each part's significant height, as high as it's taken to run.
    const float2 swash = scale * float2(sqrt(0.563) * beach, sqrt(0.004)) / 2.0;
    level.swash = swash * saturate(1.0 - (under / max(2.0 * (swash.x + swash.y), 1e-3)));
    const float incident = waves.swash_incident.w + dot(waves.swash_incident.xyz, point);
    const float surging = waves.swash_surging.w + dot(waves.swash_surging.xyz, point);
    level.turning = float4(cos(incident), sin(incident), cos(surging), sin(surging));
    return level;
}

// How high it stands now.
public float level_now(ShoreLevel level) {
    return level.mean + (level.swash.x * level.turning.x) + (level.swash.y * level.turning.z);
}

// The highest the waves' swash ran `ago` seconds before: as high as each wave runs, on the
// surging of their groups as it stood then.
public float swash_high(Waves waves, ShoreLevel level, float ago) {
    float s;
    float c;
    sincos(waves.run_up.w * ago, s, c);
    return level.mean + level.swash.x +
           (level.swash.y * ((level.turning.z * c) + (level.turning.w * s)));
}

// The sea's floor under a point: x how deep the water over it is at rest, and yz how fast it
// deepens along the eye's first and second ways across, metres per metre.
public float3 deep_floor() {
    return float3(1.0e6, 0.0, 0.0);
}

// Of the water at a point of sea level, where the waves move it, how its surface faces there, and
// how rough with waves too short to draw it is: their mean square slope. Taken in the frame under
// the eye. And of the waves that break, how much they bunch the water there, and the mean square of
// that of those too short to draw. And how high the drawn waves rise there as seen, and how high
// they stood a quarter of their period before; the mean square of that, all told and of what's not
// seen; and the mean square of its slope as seen.
public struct WaveSurface {
    public float3 moved;
    public float3 normal;
    public float roughness;
    public float breaking;
    public float breaking_left;
    public float rise;
    public float risen;
    public float heave;
    public float heave_unseen;
    public float rise_slope;
};

float square(float x) {
    return x * x;
}

// Near enough how often a standard normal value is under `x`: Bowling and others' logistic fit.
float below(float x) {
    return 1.0 / (1.0 + exp(-x * (1.5976 + (0.07056 * x * x))));
}

// At `rest`, a point of sea level from the eye, seen over `footprint` metres, over `floor`: as
// hog::waves::surface_at, but for the waves shorter than the footprint, which only roughen it.
public WaveSurface wave_surface(Waves waves, float3 rest, float footprint, float3 floor) {
    const float3 up = waves.up_height.xyz;
    const float3 first = waves.first.xyz;
    const float3 second = waves.second.xyz;
    WaveSurface surface;
    surface.roughness = waves.count_left_radius.y;
    float rising = 0.0;
    float risen = 0.0;
    float2 carried = float2(0.0);
    float2 slope = float2(0.0);
    // Symmetric: xx, xy and yy.
    float3 bunching = float3(0.0);
    surface.breaking = 0.0;
    surface.breaking_left = waves.foam.y;
    surface.heave = 0.0;
    surface.heave_unseen = 0.0;
    surface.rise_slope = 0.0;
    const float shown_from = 1.0 / (footprint * footprints_shown / 2.0);
    const int count = int(waves.count_left_radius.x);
    // Where it's shallow enough for any wave to feel the floor.
    const float depth = max(floor.x, shallowest);
    const bool shallow = depth < waves.count_left_radius.w;
    const float2 deepening = floor.yz;
    const float steepness = length(deepening);
    const float2 offshore = steepness > 0.0 ? deepening / steepness : float2(0.0);
    // Of the waves drawn, the mean square slope and breaking bunching of what's not seen.
    float drawn_roughness = 0.0;
    float drawn_breaking = 0.0;
    for (int i = 0; i < count; ++i) {
        const float4 heading = waves.waves[i].heading_wavenumber;
        const float4 shape = waves.waves[i].amplitude_phase_slope_heave;
        const float4 along = waves.waves[i].along;
        float full = shape.x;
        float wavenumber = along.z;
        float2 ahead = along.xy;
        float phase = (heading.w * dot(heading.xyz, rest)) + shape.y;
        float stretch = 1.0;
        const float deep = along.z;
        const float kappa = deep * depth;
        if (shallow && kappa < deep_enough) {
            // As hog::waves::surface_at.
            const float kh = depth_wavenumber(kappa);
            // tanh(kh), as the dispersion relation has it.
            const float thinned = kappa / kh;
            const float group = 0.5 * (1.0 + (kh * (1.0 - (thinned * thinned)) / thinned));
            const float towards = saturate((0.3 - dot(ahead, offshore)) / 0.6);
            const float sheltered = towards * towards * (3.0 - (2.0 * towards));
            full *= sqrt(1.0 / (2.0 * group * thinned)) * lerp(sheltered, 1.0, thinned);
            stretch = 1.0 / thinned;
            const float2 lag = lag_at(waves.waves[i].breaking.yzw, kappa);
            phase += lag.x;
            const float2 turned = (ahead * deep) + (deepening * (deep * lag.y));
            wavenumber = length(turned);
            ahead = wavenumber > 0.0 ? turned / wavenumber : ahead;
        }
        const float kept = saturate((full_turn / wavenumber * shown_from) - 1.0);
        if (kept <= 0.0) {
            surface.roughness += shape.z;
            surface.breaking_left += waves.waves[i].breaking.x;
            // The shorter waves shoal less than this one, which is near enough for them.
            const float shoaled = shape.x > 0.0 ? full / shape.x : 1.0;
            surface.heave_unseen += shape.w * shoaled * shoaled;
            break;
        }
        const float steep_full = full * wavenumber;
        const float unseen = (1.0 - (kept * kept)) / 2.0;
        drawn_roughness += steep_full * steep_full * unseen;
        const float breaking_full = steep_full * along.w;
        drawn_breaking += breaking_full * breaking_full * unseen;
        surface.heave_unseen += full * full * unseen;
        const float amplitude = full * kept;
        const float steep = steep_full * kept;
        surface.heave += amplitude * amplitude / 2.0;
        surface.rise_slope += steep * steep / 2.0;
        float s;
        float c;
        sincos(phase, s, c);
        rising += amplitude * c;
        risen -= amplitude * s;
        carried -= ahead * (choppiness * amplitude * stretch * s);
        slope += ahead * (steep * s);
        bunching += float3(ahead.x * ahead.x, ahead.x * ahead.y, ahead.y * ahead.y) *
                    (choppiness * steep * stretch * c);
        surface.breaking += choppiness * steep * stretch * c * along.w;
    }
    // Those too high for the depth break, short or long, as many as it takes to bring them down to
    // what it holds: as hog::waves::surface_at, and all that's summed of them scales with them.
    const float all_heave = surface.heave + surface.heave_unseen;
    const float held =
        shallow && all_heave > 0.0 ? min(breaking_depth * depth / sqrt(8.0 * all_heave), 1.0) : 1.0;
    const float held_squared = held * held;
    rising *= held;
    carried *= held;
    slope *= held;
    bunching *= held;
    surface.breaking *= held;
    surface.roughness += drawn_roughness * held_squared;
    surface.breaking_left += drawn_breaking * held_squared;
    surface.heave = all_heave * held_squared;
    surface.heave_unseen *= held_squared;
    surface.rise_slope *= held_squared;
    surface.rise = rising;
    surface.risen = risen * held;
    surface.moved = (up * rising) + (first * carried.x) + (second * carried.y);
    const float3 one = (first * (1.0 - bunching.x)) - (second * bunching.y) - (up * slope.x);
    const float3 two = (second * (1.0 - bunching.z)) - (first * bunching.y) - (up * slope.y);
    surface.normal = normalize(cross(one, two));
    return surface;
}

// The foam's pattern at a point of sea level, as much of it as is seen: of its coarse waves and its
// fine, fields of unit variance of what's seen of them, the share of their variance that's seen,
// and the mean square of their slope, per metre; and another of its coarse waves, a quarter turn
// on.
public struct FoamPattern {
    public float coarse;
    public float fine;
    public float across;
    public float coarse_seen;
    public float fine_seen;
    public float coarse_slope;
    public float fine_slope;
};

// Adds the pattern's waves from `first` to before `last` to `pattern`, at `rest`, a point of sea
// level from the eye, seen over `footprint` metres.
void add_foam_waves(Waves waves, inout FoamPattern pattern, float3 rest, float footprint, int first,
                    int last) {
    const float shown_from = 1.0 / (footprint * footprints_shown / 2.0);
    for (int i = first; i < last; ++i) {
        const FoamWave wave = waves.foam_pattern[i];
        const float length = wave.amplitude_length.y;
        const float amplitude = wave.amplitude_length.x * saturate((length * shown_from) - 1.0);
        if (amplitude <= 0.0) {
            continue;
        }
        float s;
        float c;
        sincos(dot(wave.wavenumber_phase.xyz, rest) + wave.wavenumber_phase.w, s, c);
        const float seen = amplitude * amplitude / 2.0;
        const float slope = seen * square(wave.amplitude_length.z);
        if (i < coarse_foam) {
            pattern.coarse += amplitude * c;
            pattern.across += amplitude * s;
            pattern.coarse_seen += seen;
            pattern.coarse_slope += slope;
        } else {
            pattern.fine += amplitude * c;
            pattern.fine_seen += seen;
            pattern.fine_slope += slope;
        }
    }
}

// How much of the sea is white where `water` lies: breaking, and with foam left; how far over
// where they break the waves are, in standard deviations of what places it; and how fresh what's
// left is, from 0 to 1.
public struct Foam {
    public float breaking;
    public float left;
    public float over;
    public float fresh;
};

// How far the bunching of the waves that break rises over where they break, with the whole
// pattern standing in for what the waves too short to draw add, and how the whitecaps churn.
float breaking_over(Waves waves, WaveSurface water, FoamPattern pattern) {
    return water.breaking + (sqrt(water.breaking_left / 2.0) * (pattern.coarse + pattern.fine)) +
           (waves.foam_left.w * pattern.across) - waves.foam.x;
}

// Foam fades out over its edges, not at a line: over this much of the spread of what places them,
// less for whitecaps than for the foam they leave; and how much breaking crests churn, to that of
// what places them. As ocean.cpp has them.
static const float breaking_edges = 0.12;
static const float foam_edges = 0.3;
static const float foam_ragged = 0.4;

// Of what's white in the surf, the share of it breaking, as hog::waves::surf has it; and over how
// much of the waves' period the foam they leave fades out at its edges.
static const float surf_breaking = 0.25;
static const float trailing_edges = 0.04;

// How many standard deviations above its mean a normal value exceeds `share` of the time, as
// `below` has it: so the root of a cubic in it.
float exceeding(float share) {
    const float within = clamp(share, 1e-6, 1.0 - 1e-6);
    const float logit = log((1.0 - within) / within);
    const float u = 0.3417661882 * abs(logit);
    return sign(logit) * 5.4944485142 * sinh(log(u + sqrt((u * u) + 1.0)) / 3.0);
}

// Where the waves break in the surf: x how far over their mean the drawn waves must rise there for
// them to break white, in standard deviations; y how much of each wave's period behind its crest
// the foam they leave lies in, and z how near the foam pattern's fine field's mean the lace in it
// lies, in standard deviations; w the share of the sea white in the surf.
public struct Surf {
    public float breaking;
    public float trailing;
    public float lace;
    public float white;
};

// The surf where `water` lies over `floor`: smooth enough to take between points a few pixels
// apart.
public Surf surf_at(Waves waves, WaveSurface water, float3 floor) {
    Surf surf;
    surf.breaking = 0.0;
    surf.trailing = 0.0;
    surf.lace = 0.0;
    surf.white = 0.0;
    const float depth = max(floor.x, shallowest);
    if (depth >= waves.count_left_radius.w) {
        return surf;
    }
    // As hog::waves::breaking_waves, of the drawn waves' root-mean-square height.
    const float over = min(sqrt(8.0 * water.heave) / (breaking_depth * depth), 1.0);
    const float squared = over * over;
    surf.white = squared * squared * (1.0 - exp(-squared));
    const float breaking = surf.white * surf_breaking;
    surf.breaking = exceeding(breaking);
    // What's left trails the crests, as a lace, so wide as to cover as much as is left in all;
    // the lace's share found as blurred over the foam's edges, as ocean.cpp has the wind's.
    const float trailing = sqrt(surf.white - breaking);
    surf.trailing = trailing;
    surf.lace = sqrt(1.0 + square(foam_edges)) * exceeding((1.0 - trailing) / 2.0);
    return surf;
}

// How far the drawn waves rise over where they break in the surf, and how the rollers churn.
float surf_over(WaveSurface water, Surf surf, FoamPattern pattern) {
    const float heave = sqrt(water.heave);
    const float blurred = sqrt(1.0 + square(foam_ragged) + square(breaking_edges));
    return water.rise + (foam_ragged * heave * pattern.across) - (surf.breaking * heave * blurred);
}

// How fresh foam left is where it's not known how long ago it was left.
static const float middling_fresh = 0.5;

// How likely the water is to lie where the foam left trails the crests, and how fresh that is: by
// how far through the waves' period it is since a crest passed, as the drawn waves sum, where
// they're seen.
float2 trailing_crests(WaveSurface water, Surf surf, float footprint) {
    const float seen_heave = water.heave - water.heave_unseen;
    if (!(seen_heave > 0.0) || (water.rise == 0.0 && water.risen == 0.0)) {
        return float2(surf.trailing, middling_fresh);
    }
    const float seen = saturate(seen_heave / water.heave);
    const float since = frac(atan2(water.risen, water.rise) / full_turn);
    // From the middle of the band behind the crest, in periods.
    const float off = abs(frac(since - (surf.trailing / 2.0) + 0.5) - 0.5);
    const float wavenumber = sqrt(water.rise_slope / seen_heave);
    const float edges = trailing_edges + (footprint * wavenumber / full_turn);
    const float fresh = saturate(1.0 - (since / max(surf.trailing, 1e-6)));
    return lerp(float2(surf.trailing, middling_fresh),
                float2(below(((surf.trailing / 2.0) - off) / edges), fresh), seen);
}

// How that spreads where the waves and the pattern aren't seen, or change across the footprint.
float surf_spread_at(WaveSurface water, FoamPattern pattern, float footprint) {
    const float coarse_unseen = max(1.0 - pattern.coarse_seen, 0.0);
    const float churning = square(foam_ragged) * water.heave;
    const float hard = sqrt(water.heave_unseen + (churning * coarse_unseen) +
                            (square(breaking_edges) * water.heave));
    const float soft = footprint * sqrt(water.rise_slope + (churning * pattern.coarse_slope));
    return max(max(hard, soft), 1e-6);
}

// How far apart, in their spreads, values may lie and still each count.
static const float reaching = 3.0;

// How the waves' breaking and the pattern's coarse field spread where its fine field isn't seen.
float breaking_spread_at(Waves waves, WaveSurface water, FoamPattern pattern) {
    const float coarse_unseen = max(1.0 - pattern.coarse_seen, 0.0);
    const float fine_unseen = max(1.0 - pattern.fine_seen, 0.0);
    return sqrt((water.breaking_left * (coarse_unseen + fine_unseen) / 2.0) +
                (square(waves.foam_left.w) * coarse_unseen) +
                square(breaking_edges * waves.foam_left.z));
}

float across_spread_at(FoamPattern pattern, float footprint) {
    return max(sqrt(max(1.0 - pattern.coarse_seen, 0.0) + square(foam_edges)),
               footprint * sqrt(pattern.coarse_slope));
}

// The pattern's coarse waves at `rest`, a point of sea level from the eye, seen over `footprint`
// metres: smooth enough to take between points a few pixels apart.
public FoamPattern coarse_foam_at(Waves waves, float3 rest, float footprint) {
    FoamPattern pattern;
    pattern.coarse = 0.0;
    pattern.fine = 0.0;
    pattern.across = 0.0;
    pattern.coarse_seen = 0.0;
    pattern.fine_seen = 0.0;
    pattern.coarse_slope = 0.0;
    pattern.fine_slope = 0.0;
    add_foam_waves(waves, pattern, rest, footprint, 0, coarse_foam);
    return pattern;
}

// To `coarse`, the pattern's coarse waves where `water` lies, adds its fine waves at `rest`, seen
// over `footprint` metres, where they might make any of it white.
public FoamPattern foam_pattern_at(Waves waves, WaveSurface water, Surf surf, FoamPattern coarse,
                                   float3 rest, float footprint) {
    FoamPattern pattern = coarse;
    const float fine_reach = reaching * sqrt(water.breaking_left / 2.0);
    const bool near_breaking =
        waves.foam.z > 0.0 &&
        breaking_over(waves, water, pattern) >
            -fine_reach - (reaching * breaking_spread_at(waves, water, pattern));
    const bool in_patch =
        pattern.across - waves.foam_left.x > -reaching * across_spread_at(pattern, footprint);
    if (near_breaking || surf.white > 0.0 || in_patch) {
        add_foam_waves(waves, pattern, rest, footprint, coarse_foam, foam_waves);
    }
    return pattern;
}

// Where the waves and the pattern are seen, whether the sea is white there; where not, how likely.
public Foam foam_at(Waves waves, WaveSurface water, Surf surf, FoamPattern pattern,
                    float footprint) {
    Foam foam;
    foam.breaking = 0.0;
    foam.left = 0.0;
    foam.over = -reaching;
    foam.fresh = middling_fresh;
    if (waves.foam.z <= 0.0 && surf.white <= 0.0) {
        return foam;
    }
    if (waves.foam.z > 0.0) {
        // Blurred over how much each changes across what's seen.
        const float half_left = water.breaking_left / 2.0;
        const float breaking_soft =
            footprint * sqrt((half_left * (pattern.coarse_slope + pattern.fine_slope)) +
                             (square(waves.foam_left.w) * pattern.coarse_slope));
        const float breaking_spread =
            max(max(breaking_spread_at(waves, water, pattern), breaking_soft), 1e-6);
        const float over = breaking_over(waves, water, pattern);
        foam.breaking = below(over / breaking_spread);
        foam.over = over / waves.foam_left.z;
    }
    if (surf.white > 0.0) {
        const float over = surf_over(water, surf, pattern);
        foam.breaking =
            max(foam.breaking, below(over / surf_spread_at(water, pattern, footprint)));
        foam.over = max(foam.over, over / max(sqrt(water.heave), 1e-6));
    }
    const float fine_spread = max(sqrt(max(1.0 - pattern.fine_seen, 0.0) + square(foam_edges)),
                                  footprint * sqrt(pattern.fine_slope));
    if (waves.foam.z > 0.0) {
        const float lace = waves.foam_left.y;
        const float in_lace = below((lace - pattern.fine) / fine_spread) -
                              below((-lace - pattern.fine) / fine_spread);
        foam.left = in_lace * below((pattern.across - waves.foam_left.x) /
                                    across_spread_at(pattern, footprint));
    }
    if (surf.white > 0.0) {
        const float in_lace = below((surf.lace - pattern.fine) / fine_spread) -
                              below((-surf.lace - pattern.fine) / fine_spread);
        const float2 trailing = trailing_crests(water, surf, footprint);
        if (in_lace * trailing.x > foam.left) {
            foam.left = in_lace * trailing.x;
            foam.fresh = trailing.y;
        }
    }
    return foam;
}
