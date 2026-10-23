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

// Of a lattice point's gradient, one of the cube's corners picked by its hash, how far along it
// `offset` from the point lies.
float along_gradient(uint hash, float3 offset) {
    const uint picked = hashed(hash);
    return dot(float3((picked & 1u) != 0u ? 1.0 : -1.0, (picked & 2u) != 0u ? 1.0 : -1.0,
                      (picked & 4u) != 0u ? 1.0 : -1.0),
               offset);
}

// Gradient noise, about -1 to 1, over a lattice of `cells` to the period; `at` in cells.
float noise(float3 at, int cells, uint octave) {
    const float3 below = floor(at);
    const float3 within = at - below;
    // The lattice points either side on each axis, wrapped to the period, each axis scrambled
    // apart so a single hash tells the corners apart.
    const int3 low = ((int3(below) % cells) + cells) % cells;
    const int3 high = int3(low.x + 1 == cells ? 0 : low.x + 1, low.y + 1 == cells ? 0 : low.y + 1,
                           low.z + 1 == cells ? 0 : low.z + 1);
    const uint3 scramble = uint3(0x8da6b343u, 0xd8163841u, 0xcb1ab31fu);
    const uint3 lows = uint3(low) * scramble;
    const uint3 highs = uint3(high) * scramble;
    const uint seed = octave * 0x9e3779b9u;
    const float3 eased = within * within * within * ((within * ((within * 6.0) - 15.0)) + 10.0);
    const float3 rest = within - 1.0;
    const float4 x_low = float4(
        along_gradient(seed ^ lows.x ^ lows.y ^ lows.z, float3(within.x, within.y, within.z)),
        along_gradient(seed ^ lows.x ^ highs.y ^ lows.z, float3(within.x, rest.y, within.z)),
        along_gradient(seed ^ lows.x ^ lows.y ^ highs.z, float3(within.x, within.y, rest.z)),
        along_gradient(seed ^ lows.x ^ highs.y ^ highs.z, float3(within.x, rest.y, rest.z)));
    const float4 x_high = float4(
        along_gradient(seed ^ highs.x ^ lows.y ^ lows.z, float3(rest.x, within.y, within.z)),
        along_gradient(seed ^ highs.x ^ highs.y ^ lows.z, float3(rest.x, rest.y, within.z)),
        along_gradient(seed ^ highs.x ^ lows.y ^ highs.z, float3(rest.x, within.y, rest.z)),
        along_gradient(seed ^ highs.x ^ highs.y ^ highs.z, float3(rest.x, rest.y, rest.z)));
    const float4 along_x = lerp(x_low, x_high, eased.x);
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

// How fast the wind blows at `gust`, as gust_at has it, where it blows `speed` on the whole:
// slackening in the lulls, and harder in the heart of a gust.
public float gusting(float speed, float gust) {
    return speed * lerp(0.55, 1.5, gust);
}
