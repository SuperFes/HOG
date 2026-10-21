module waves;

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

// As hog::render's kept_samples and kept_deepest, and hog::waves' deep_enough and the shore's
// profile in waves/shoaling.hpp.
static const int kept_samples = 32;
static const float kept_deepest = 64.0;
static const float deep_enough = 3.14159265358979;
static const float dean_scale = 0.12;
static const float shelf_run = 100.0;
static const float shelf_depth = shelf_run * shelf_run * dean_scale * dean_scale * dean_scale;
static const float shallowest = 0.01;
static const float full_turn = 6.283185307;

// Each as it runs round the eye: over the few kilometres seen, the planet's up turns too little to
// change how high a wave shows or which way it runs along the ground.
public struct Wave {
    // xyz unit, in the planet's frame; w the wavenumber, rad/m.
    public float4 heading_wavenumber;
    // x metres high, y its phase through the eye, z the mean square slope of it and every shorter
    // wave, w its length.
    public float4 amplitude_phase_slope_length;
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
    // How much of the waves runs where the water's shallow, against the root of its depth.
    public float4 kept[kept_samples / 4];
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

// How much of the waves runs `depth` deep, once those too high for it have broken.
float kept_at(Waves waves, float depth) {
    const float at = sqrt(max(depth, 0.0) / kept_deepest) * float(kept_samples - 1);
    if (at >= float(kept_samples - 1)) {
        return 1.0;
    }
    const int below = int(at);
    const float low = waves.kept[below / 4][below % 4];
    const float high = waves.kept[(below + 1) / 4][(below + 1) % 4];
    return lerp(low, high, at - float(below));
}

// The sea's floor under a point: x how deep the water over it is at rest, and yz how fast it
// deepens along the eye's first and second ways across, metres per metre.
public float3 deep_floor() {
    return float3(1.0e6, 0.0, 0.0);
}

// Of the water at a point of sea level, where the waves move it, how its surface faces there, and
// how rough with waves too short to draw it is: their mean square slope. Taken in the frame under
// the eye. And of the waves that break, how much they bunch the water there, and the mean square of
// that of those too short to draw.
public struct WaveSurface {
    public float3 moved;
    public float3 normal;
    public float roughness;
    public float breaking;
    public float breaking_left;
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
    float2 carried = float2(0.0);
    float2 slope = float2(0.0);
    // Symmetric: xx, xy and yy.
    float3 bunching = float3(0.0);
    surface.breaking = 0.0;
    surface.breaking_left = waves.foam.y;
    const float shown_from = 1.0 / (footprint * footprints_shown / 2.0);
    const int count = int(waves.count_left_radius.x);
    // Where it's shallow enough for any wave to feel the floor.
    const float depth = max(floor.x, shallowest);
    const bool shallow = depth < waves.count_left_radius.w;
    const float2 deepening = floor.yz;
    const float steepness = length(deepening);
    const float2 offshore = steepness > 0.0 ? deepening / steepness : float2(0.0);
    const float kept_shallow = shallow ? kept_at(waves, depth) : 1.0;
    for (int i = 0; i < count; ++i) {
        const float4 heading = waves.waves[i].heading_wavenumber;
        const float4 shape = waves.waves[i].amplitude_phase_slope_length;
        const float4 along = waves.waves[i].along;
        // Those too high for the depth break, short or long.
        float full = shape.x * kept_shallow;
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
            break;
        }
        const float steep_full = full * wavenumber;
        const float unseen = (1.0 - (kept * kept)) / 2.0;
        surface.roughness += steep_full * steep_full * unseen;
        const float breaking_full = steep_full * along.w;
        surface.breaking_left += breaking_full * breaking_full * unseen;
        const float amplitude = full * kept;
        const float steep = steep_full * kept;
        float s;
        float c;
        sincos(phase, s, c);
        rising += amplitude * c;
        carried -= ahead * (choppiness * amplitude * stretch * s);
        slope += ahead * (steep * s);
        bunching += float3(ahead.x * ahead.x, ahead.x * ahead.y, ahead.y * ahead.y) *
                    (choppiness * steep * stretch * c);
        surface.breaking += choppiness * steep * stretch * c * along.w;
    }
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

// How much of the sea is white where `water` lies: breaking, and with foam left.
public struct Foam {
    public float breaking;
    public float left;
};

// How far the bunching of the waves that break rises over where they break, with the whole
// pattern standing in for what the waves too short to draw add, and how the whitecaps churn.
public float breaking_over(Waves waves, WaveSurface water, FoamPattern pattern) {
    return water.breaking + (sqrt(water.breaking_left / 2.0) * (pattern.coarse + pattern.fine)) +
           (waves.foam_left.w * pattern.across) - waves.foam.x;
}

// Foam fades out over its edges, not at a line: over this much of the spread of what places them,
// less for whitecaps than for the foam they leave. As ocean.cpp has it.
static const float breaking_edges = 0.12;
static const float foam_edges = 0.3;

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
public FoamPattern foam_pattern_at(Waves waves, WaveSurface water, FoamPattern coarse, float3 rest,
                                   float footprint) {
    FoamPattern pattern = coarse;
    const float fine_reach = reaching * sqrt(water.breaking_left / 2.0);
    const bool near_breaking = breaking_over(waves, water, pattern) >
                               -fine_reach - (reaching * breaking_spread_at(waves, water, pattern));
    const bool in_patch = pattern.across - waves.foam_left.x >
                          -reaching * across_spread_at(pattern, footprint);
    if (near_breaking || in_patch) {
        add_foam_waves(waves, pattern, rest, footprint, coarse_foam, foam_waves);
    }
    return pattern;
}

// Where the waves and the pattern are seen, whether the sea is white there; where not, how likely.
public Foam foam_at(Waves waves, WaveSurface water, FoamPattern pattern, float footprint) {
    Foam foam;
    if (waves.foam.z <= 0.0) {
        foam.breaking = 0.0;
        foam.left = 0.0;
        return foam;
    }
    // Blurred over how much each changes across what's seen.
    const float half_left = water.breaking_left / 2.0;
    const float breaking_soft =
        footprint * sqrt((half_left * (pattern.coarse_slope + pattern.fine_slope)) +
                         (square(waves.foam_left.w) * pattern.coarse_slope));
    const float breaking_spread =
        max(max(breaking_spread_at(waves, water, pattern), breaking_soft), 1e-6);
    foam.breaking = below(breaking_over(waves, water, pattern) / breaking_spread);
    const float fine_spread = max(sqrt(max(1.0 - pattern.fine_seen, 0.0) + square(foam_edges)),
                                  footprint * sqrt(pattern.fine_slope));
    const float lace = waves.foam_left.y;
    const float in_lace = below((lace - pattern.fine) / fine_spread) -
                          below((-lace - pattern.fine) / fine_spread);
    foam.left = in_lace *
                below((pattern.across - waves.foam_left.x) / across_spread_at(pattern, footprint));
    return foam;
}
