module water;

import atmosphere;
import bend;

// Seawater, as libs/terrain's sea.hpp has it: change both together.

// Reflected straight back, as water does.
public static const float facing_reflection = 0.02;
// Of clear seawater, by the metre of it light passes through, and the light it scatters back.
public static const float3 water_absorbing = float3(0.35, 0.07, 0.03);
public static const float3 water_colour = float3(0.0, 0.15, 0.25);
// The most glancing a view down through the water is taken as, for the length of its path there.
public static const float least_facing = 0.1;

// Of light passing from air into water, the ratio of their refractive indices.
public static const float into_water = 1.0 / 1.333;

// How far a point `under` metres beyond the surface along `looking` seems to move as waves tilt
// the surface's `normal` from `level`'s: as the line of sight bends through it, against as it bends
// through still water.
public float3 bent_by_waves(float3 looking, float3 normal, float3 level, float under) {
    return (refract(looking, normal, into_water) - refract(looking, level, into_water)) * under;
}

// Schlick's: how much is reflected, more the more glancing the view. From under the sea, its
// surface reflects as much back down.
public float reflected_at(float3 looking, float3 normal) {
    const float facing = abs(dot(looking, normal));
    return facing_reflection + ((1.0 - facing_reflection) * pow(1.0 - facing, 5.0));
}

// How much of the sun's light water facing `normal`, its mean square slope `roughness` too fine to
// draw, glints towards the eye, but for the reflection: a normalised Blinn-Phong lobe as wide as it's
// rough.
public float glint_lobe(float3 looking, float3 normal, float roughness, float3 sun) {
    const float sharpness = (2.0 / roughness) - 2.0;
    const float3 halfway = normalize(sun - looking);
    const float lit = saturate(dot(normal, sun) * 10.0);
    return lit * ((sharpness + 8.0) / (8.0 * pi)) * pow(saturate(dot(normal, halfway)), sharpness);
}

// Whether the sea's own pass lays the water over what's drawn beneath it, seen from above, as
// Lighting's reach.w has it; then what's drawn there is drawn as in air.
public bool sea_lays_water(Lighting lighting) {
    return lighting.reach.w > 0.0;
}

// Whether a point `from_eye` lies in the water, as the plane of the sea's surface at the eye has
// it: true to the waves within a hand of the eye, as the near plane is.
public bool in_water(Lighting lighting, float3 from_eye) {
    return dot(from_eye, lighting.water.xyz) + lighting.water.w < 0.0;
}

// Whether a pixel at `ndc`, the picture's own coordinates, is seen from in the water: where the near
// plane there is under the sea's surface.
public bool seen_from_water(Lighting lighting, float2 ndc) {
    const float4 near = mul(lighting.eye_from_clip, float4(ndc, 1.0, 1.0));
    return in_water(lighting, near.xyz / near.w);
}

// `lit` at a point `depth` metres under the sea (negative above it), `along` metres from an eye
// `eye_height` above sea level, looking down at it `looking_down`, the cosine from straight down:
// dimmed by the water the light passes down to it and back to the eye, and filled in by what
// that water scatters of `on_water`, the light falling on the sea. From above, the way through
// the water is the depth over how steeply the eye looks down; and where the sea's pass lays the
// water, `laid`, from over it or under it, it's left as it is.
public float3 through_water(float3 lit, float3 on_water, float depth, float eye_height,
                            float along, float looking_down, bool laid = false) {
    if (laid || (depth <= 0.0 && eye_height >= 0.0)) {
        return lit;
    }
    float seen_through;
    if (eye_height >= 0.0) {
        seen_through = depth / max(looking_down, least_facing);
    } else if (depth >= 0.0) {
        seen_through = along;
    } else {
        // Of the line up out of the water, the part under its surface.
        seen_through = along * eye_height / (eye_height + depth);
    }
    const float3 passed = exp(-water_absorbing * (max(depth, 0.0) + seen_through));
    return (lit * passed) + (water_colour * on_water * (1.0 - passed));
}

// Of light passing through the water from a point `deep` metres under its surface, `along` metres
// on, sinking `sinking` metres for each (rising if negative): the share let through, and what the
// water scatters on to the end of the way of `on_water`, the light falling on its surface, which
// is dimmed by the water it passes down through to where it's scattered.
public struct WaterBetween {
    public float3 passed;
    public float3 scattered;
};

public WaterBetween water_between(float3 on_water, float deep, float sinking, float along) {
    // Rising, it's in the water only as far as the surface.
    const float under = sinking < 0.0 ? min(along, deep / -sinking) : along;
    const float steepness = 1.0 + sinking;
    const float3 reached = 1.0 - exp(-water_absorbing * steepness * under);
    WaterBetween between;
    between.passed = exp(-water_absorbing * under);
    between.scattered = water_colour * on_water * exp(-water_absorbing * deep) *
                        (steepness > 1e-3 ? reached / steepness : water_absorbing * under);
    return between;
}

// As through_water, for a point `from_eye` on the planet of `radius`, under an eye `up_height` as
// bend.slang takes it.
public float3 through_water_at(float3 lit, float3 on_water, float3 from_eye, float4 up_height,
                               float radius, bool laid) {
    const float3 up = normalize(from_eye + (up_height.xyz * (radius + up_height.w)));
    return through_water(lit, on_water, -height_above_sea(from_eye, up_height, radius),
                         up_height.w, length(from_eye), dot(-normalize(from_eye), up), laid);
}
