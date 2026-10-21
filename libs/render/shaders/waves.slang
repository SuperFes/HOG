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
    public Wave waves[drawn_waves];
};

// Of the water at a point of sea level, where the waves move it, how its surface faces there, and
// how rough with waves too short to draw it is: their mean square slope. Taken in the frame under
// the eye.
public struct WaveSurface {
    public float3 moved;
    public float3 normal;
    public float roughness;
};

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
    const float shown_from = 1.0 / (footprint * footprints_shown / 2.0);
    const int count = int(waves.count_left_radius.x);
    for (int i = 0; i < count; ++i) {
        const Wave wave = waves.waves[i];
        const float4 shape = wave.amplitude_phase_slope_length;
        const float kept = saturate((shape.w * shown_from) - 1.0);
        if (kept <= 0.0) {
            surface.roughness += shape.z;
            break;
        }
        const float full = shape.x;
        const float steep_full = full * wave.along.z;
        surface.roughness += steep_full * steep_full * (1.0 - (kept * kept)) / 2.0;
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
    }
    surface.moved = (up * rising) + (first * carried.x) + (second * carried.y);
    const float3 one = (first * (1.0 - bunching.x)) - (second * bunching.y) - (up * slope.x);
    const float3 two = (second * (1.0 - bunching.z)) - (first * bunching.y) - (up * slope.y);
    surface.normal = normalize(cross(one, two));
    return surface;
}
