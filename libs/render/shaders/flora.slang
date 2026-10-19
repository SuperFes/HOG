module flora;

// Shared ground for the plants grown on the terrain's patches: the hash that roots each one to
// its lattice cell, and the stand field that gathers trees into woods with edges and clearings,
// the same on every machine and at every terrain level.

public uint hashed(uint at) {
    at ^= at >> 16;
    at *= 0x7feb352du;
    at ^= at >> 15;
    at *= 0x846ca68bu;
    at ^= at >> 16;
    return at;
}

public float unit(uint at) {
    return float(hashed(at) & 0xffffu) / 65536.0;
}

float stand_corner(uint2 base, uint face, uint salt) {
    return unit(hashed(base.x ^ (face * 0x9e3779b9u) ^ salt) + base.y);
}

// Value noise over the trees' lattice, cells gathered `period_log` levels up.
float stand_octave(float2 cell, uint face, uint period_log, uint salt) {
    const float2 at = cell / float(1u << period_log);
    const float2 floored = floor(at);
    float2 f = at - floored;
    f = f * f * (3.0 - (2.0 * f));
    const uint2 base = uint2(floored);
    const float c00 = stand_corner(base, face, salt);
    const float c10 = stand_corner(base + uint2(1, 0), face, salt);
    const float c01 = stand_corner(base + uint2(0, 1), face, salt);
    const float c11 = stand_corner(base + uint2(1, 1), face, salt);
    return lerp(lerp(c00, c01, f.y), lerp(c10, c11, f.y), f.x);
}

// How deep into woodland a spot lies, 0 to 1, over `cell` given in the trees' lattice: broad
// stands with clearings between, edged by a finer grain.
public float stand_field(float2 cell, uint face) {
    return (0.65 * stand_octave(cell, face, 6u, 0x51a9du)) +
           (0.35 * stand_octave(cell, face, 4u, 0xb07a7u));
}
