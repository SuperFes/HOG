module gusts;

// Gusts: patches of stronger wind tens of metres across, with lulls between, drifting downwind over
// the ground and changing shape as they go. A field in the planet's frame that repeats over its
// period, placed about the eye as render::gust_anchor has it.

// Metres the field repeats over, as render::gust_period.
static const float period = 2048.0;

uint hashed(uint at) {
    at ^= at >> 16;
    at *= 0x7feb352du;
    at ^= at >> 15;
    at *= 0x846ca68bu;
    at ^= at >> 16;
    return at;
}

// One of the twelve edges of a cube, at a lattice point wrapped to the field's period.
float3 gradient(int3 lattice, int cells, uint octave) {
    const int3 at = ((lattice % cells) + cells) % cells;
    const uint hash =
        hashed(uint(at.x) ^ hashed(uint(at.y) ^ hashed(uint(at.z) ^ (octave * 0x9E3779B9u))));
    const float3 edges[12] = { float3(1, 1, 0), float3(-1, 1, 0), float3(1, -1, 0),
                               float3(-1, -1, 0), float3(1, 0, 1), float3(-1, 0, 1),
                               float3(1, 0, -1), float3(-1, 0, -1), float3(0, 1, 1),
                               float3(0, -1, 1), float3(0, 1, -1), float3(0, -1, -1) };
    return edges[hash % 12u];
}

// Gradient noise, about -1 to 1, over a lattice of `cells` to the period; `at` in cells.
float noise(float3 at, int cells, uint octave) {
    const float3 below = floor(at);
    const int3 lattice = int3(below);
    const float3 within = at - below;
    const float3 eased = within * within * within * ((within * ((within * 6.0) - 15.0)) + 10.0);
    float corners[8];
    for (uint corner = 0; corner < 8; ++corner) {
        const int3 step = int3(corner & 1u, (corner >> 1) & 1u, (corner >> 2) & 1u);
        corners[corner] = dot(gradient(lattice + step, cells, octave), within - float3(step));
    }
    const float4 along_x = lerp(float4(corners[0], corners[2], corners[4], corners[6]),
                                float4(corners[1], corners[3], corners[5], corners[7]), eased.x);
    const float2 along_y = lerp(along_x.xz, along_x.yw, eased.y);
    return lerp(along_y.x, along_y.y, eased.z);
}

// How hard the gusts blow at a point given from the eye, 0 in a lull to 1 in the heart of a gust,
// with the field lying `anchor` from the eye.
public float gust_at(float3 from_eye, float3 anchor) {
    const float3 at = (from_eye + anchor) / period;
    // Lattices of uneven sizes, so their cells never line up into a grid; about 41, 17 and 8 m.
    const float broad = (0.65 * noise(at * 50.0, 50, 0u)) + (0.3 * noise(at * 117.0, 117, 1u)) +
                        (0.15 * noise(at * 263.0, 263, 2u));
    return smoothstep(-0.12, 0.22, broad);
}
