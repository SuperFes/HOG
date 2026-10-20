module water;

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

// What the water passes of the light down to ground `depth` metres under it and back up to an eye
// looking down at it `looking_down`, the cosine from straight down.
public float3 water_passed(float depth, float looking_down) {
    return exp(-water_absorbing * depth * (1.0 + (1.0 / max(looking_down, least_facing))));
}
