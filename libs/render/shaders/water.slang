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

// `lit` at a point `depth` metres under the sea (negative above it), `along` metres from an eye
// `eye_height` above sea level, looking down at it `looking_down`, the cosine from straight down:
// dimmed by the water the light passes down to it and back to the eye, and filled in by what
// that water scatters of `on_water`, the light falling on the sea. From above, the way through
// the water is the depth over how steeply the eye looks down.
public float3 through_water(float3 lit, float3 on_water, float depth, float eye_height,
                            float along, float looking_down) {
    if (depth <= 0.0 && eye_height >= 0.0) {
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

// As through_water, for a point `from_eye` on the planet of `radius`, under an eye `up_height` as
// bend.slang takes it.
public float3 through_water_at(float3 lit, float3 on_water, float3 from_eye, float4 up_height,
                               float radius) {
    const float3 up = normalize(from_eye + (up_height.xyz * (radius + up_height.w)));
    return through_water(lit, on_water, -height_above_sea(from_eye, up_height, radius),
                         up_height.w, length(from_eye), dot(-normalize(from_eye), up));
}
