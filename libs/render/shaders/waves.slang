module waves;

// As hog::render::drawn_waves: the most waves the sea is drawn with.
public static const int drawn_waves = 48;
// As hog::waves::choppiness: how much of its orbit's width the water moves along a wave as well as
// up and down.
static const float choppiness = 1.0;

// Waves fade as their length falls from this many footprints to half as many.
static const float footprints_shown = 4.0;

// Each as it runs round the eye: over the few kilometres seen, the planet's up turns too little to
// change how high a wave shows or which way it runs along the ground.
public struct Wave {
    // xyz unit, in the planet's frame; w the wavenumber, rad/m.
    public float4 heading_wavenumber;
    // x metres high, y its phase through the eye, z the mean square slope of it and every shorter
    // wave, w its length.
    public float4 amplitude_phase_slope_length;
    // xy which way it runs along the ground, in the eye's frame; z its wavenumber along it.
    public float4 along;
    // x how much it takes part in breaking white; y the mean square slope of it and every shorter
    // wave, each by the square of how much it does.
    public float4 breaking;
};

// Longest first.
public struct Waves {
    // x how many, y the mean square slope of the waves left out, z the planet's radius.
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
    public Wave waves[drawn_waves];
};

// Of the water at a point of sea level, where the waves move it, how its surface faces there, and
// how rough with waves too short to draw it is: their mean square slope. Taken in the frame under
// the eye. And how much of it is breaking white.
public struct WaveSurface {
    public float3 moved;
    public float3 normal;
    public float roughness;
    public float breaking;
};

// Near enough how often a standard normal value is under `x`: Bowling and others' logistic fit.
float below(float x) {
    return 1.0 / (1.0 + exp(-x * (1.5976 + (0.07056 * x * x))));
}

// At `rest`, a point of sea level from the eye, seen over `footprint` metres: as
// hog::waves::surface_at, but for the waves shorter than the footprint, which only roughen it.
public WaveSurface wave_surface(Waves waves, float3 rest, float footprint) {
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
    // Of the waves that break, how much they bunch the water here, and the mean square of that of
    // those too short to draw.
    float breaking = 0.0;
    float breaking_left = waves.foam.y;
    const float shown_from = 1.0 / (footprint * footprints_shown / 2.0);
    const int count = int(waves.count_left_radius.x);
    for (int i = 0; i < count; ++i) {
        const Wave wave = waves.waves[i];
        const float4 shape = wave.amplitude_phase_slope_length;
        const float kept = saturate((shape.w * shown_from) - 1.0);
        if (kept <= 0.0) {
            surface.roughness += shape.z;
            breaking_left += wave.breaking.y;
            break;
        }
        const float full = shape.x;
        const float steep_full = full * wave.along.z;
        const float unseen = (1.0 - (kept * kept)) / 2.0;
        surface.roughness += steep_full * steep_full * unseen;
        const float breaking_full = steep_full * wave.breaking.x;
        breaking_left += breaking_full * breaking_full * unseen;
        const float amplitude = full * kept;
        const float steep = steep_full * kept;
        float s;
        float c;
        sincos((wave.heading_wavenumber.w * dot(wave.heading_wavenumber.xyz, rest)) + shape.y, s, c);
        const float2 ahead = wave.along.xy;
        rising += amplitude * c;
        carried -= ahead * (choppiness * amplitude * s);
        slope += ahead * (steep * s);
        bunching += float3(ahead.x * ahead.x, ahead.x * ahead.y, ahead.y * ahead.y) *
                    (choppiness * steep * c);
        breaking += choppiness * steep * c * wave.breaking.x;
    }
    // Where it bunches most it breaks, or might where the waves too short to draw would bunch it
    // the rest of the way.
    surface.breaking =
        waves.foam.z > 0.0 ? below((breaking - waves.foam.x) / sqrt(breaking_left + 1e-8)) : 0.0;
    surface.moved = (up * rising) + (first * carried.x) + (second * carried.y);
    const float3 one = (first * (1.0 - bunching.x)) - (second * bunching.y) - (up * slope.x);
    const float3 two = (second * (1.0 - bunching.z)) - (first * bunching.y) - (up * slope.y);
    surface.normal = normalize(cross(one, two));
    return surface;
}
