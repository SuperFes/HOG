module condition;

// How a tree fares, as render::packed_foliage packs flora::Condition: change both together.

public static const uint sound = 0;
public static const uint failing = 1;
public static const uint scorched = 2;
public static const uint dead = 3;
public static const uint charred = 4;

// As render::condition_of: the high half of the last byte, anything unknown sound.
public uint condition_of(uint packed) {
    const uint condition = packed >> 28;
    return condition <= charred ? condition : sound;
}

public bool standing_dead(uint condition) {
    return condition == dead || condition == charred;
}

// Burned wood.
public static const float3 charcoal = float3(0.018, 0.016, 0.015);

// A dead tree's wood, weathered silver-grey or burned black, in the tree's own tone.
public float3 dead_wood(uint condition, float tone) {
    return (condition == charred ? charcoal : float3(0.15, 0.142, 0.132)) * tone;
}

// The share of a failing crown already shed.
public static const float shed = 0.45;

// What's left of a failing crown, dried toward a rusty brown.
public float3 failing_foliage(float3 colour) {
    return lerp(colour, float3(0.1, 0.062, 0.024), 0.6);
}

// How high a scorched survivor's trunk is blackened, of its height, by its own dice.
public float scorch_height(float dice) {
    return 0.3 + (0.15 * dice);
}
