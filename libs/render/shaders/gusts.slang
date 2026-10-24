module gusts;

// Gusts: patches of stronger wind tens of metres across and longer downwind, with lulls between,
// drifting downwind over the ground and changing shape as they go; gustier stretches some hundreds
// of metres long coming and going among calmer ones. A field that repeats over its period in axes
// of its own, downwind, across the wind and up, placed about the eye as render::Gusts has it.

// Metres the field repeats over, as render::gust_period.
public static const float period = 2048.0;

// How many times longer patches are downwind than across it, as render::gust_stretch.
static const float stretch = 2.5;

// Metres from crest to crest of the waves running downwind through swaying grass, as
// render::sway_wavelength.
static const float wavelength = 12.0;

// The gusts' field as a frame draws it, as render::GustsSeen has it: where it lies from the eye in
// its own axes, which way the wind blows, level, and which way is up.
public struct GustFrame {
    public float3 anchor;
    public float3 along;
    public float3 up;
};

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
public float periodic_noise(float3 at, int cells, uint octave) {
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

// The gusts' field at a point given from the eye: smooth, about -1 to 1, rising into the gusts.
public float gust_field(float3 from_eye, GustFrame frame) {
    const float3 axes = float3(dot(from_eye, frame.along) / stretch,
                               dot(from_eye, cross(frame.up, frame.along)), dot(from_eye, frame.up));
    const float3 at = (axes + frame.anchor) / period;
    // Lattices of uneven sizes, so their cells never line up into a grid; about 41, 17 and 8 m
    // across the wind, and gustier and calmer stretches about 230 m.
    return (0.6 * periodic_noise(at * 50.0, 50, 0u)) + (0.28 * periodic_noise(at * 117.0, 117, 1u)) +
           (0.14 * periodic_noise(at * 263.0, 263, 2u)) + (0.4 * periodic_noise(at * 9.0, 9, 3u));
}

// How hard the gusts blow where their field is `field`, 0 in a lull to 1 in the heart of a gust.
public float gust_of(float field) {
    return smoothstep(-0.12, 0.22, field);
}

public float gust_at(float3 from_eye, GustFrame frame) {
    return gust_of(gust_field(from_eye, frame));
}

// How fast the wind blows at `gust`, as gust_at has it, where it blows `speed` on the whole:
// slackening in the lulls, and harder in the heart of a gust.
public float gusting(float speed, float gust) {
    return speed * lerp(0.55, 1.5, gust);
}

// How far grass is held over by wind of `speed`, 0 standing to 1 laid flat.
public float laid_over(float speed) {
    return (speed * speed) / ((speed * speed) + 36.0);
}

// How far either way grass sways in wind of `speed`, as a share of its height: most in a breeze,
// little once the wind holds it down.
public float swaying(float speed) {
    return 0.3 * speed / (1.0 + (0.08 * speed * speed));
}

// Where in its sway grass is `from_eye`, where the gusts' field is `field`, with grass at the eye
// `wave` into its sway: in waves running downwind, their crests bent by the gusts.
public float sway_phase(float wave, float3 from_eye, GustFrame frame, float field) {
    return wave + ((2.0 * 3.14159265 / wavelength) * dot(from_eye, frame.along)) + (3.0 * field);
}

// How much of its sway grass swings through together at `gust`, as gust_of has it: the waves
// running mostly through the gusts, so they cross a field in passing bands, and blades in a lull
// stirring little.
public float swung(float gust) {
    return lerp(0.2, 1.0, gust);
}

// How much further grass is laid over at `gust` than by wind of `speed` on the whole: catching
// more light on its flanks in a gust, less in a lull.
public float gust_laid(float speed, float gust) {
    return laid_over(gusting(speed, gust)) - laid_over(speed);
}

// As gust_laid where the gusts' field is `field`, and swaying with it `phase` into its sway, as
// sway_phase has it, as a field of grass seen whole; its sway's share `seen`, less where waves are
// too fine to make out.
public float gust_sheen(float speed, float field, float phase, float seen) {
    const float gust = gust_of(field);
    return gust_laid(speed, gust) + (seen * swung(gust) * swaying(gusting(speed, gust)) * sin(phase));
}

// How a woody plant gives to the wind: held over by it as stiffly as `holds` has it, at most `lean`
// of its height, and rocking `rock` of its height either way once it blows `rocks_from` m/s, at
// `rate` radians a second and `quickening` more for each m/s on the whole.
struct Woody {
    float holds;
    float lean;
    float rock;
    float rocks_from;
    float rate;
    float quickening;
}

// Stiffer than grass by far, and trees stiffer and slower still than bushes.
static const Woody bush_wood = { 220.0, 0.25, 0.04, 6.0, 1.1, 0.2 };
static const Woody tree_wood = { 320.0, 0.16, 0.025, 8.0, 0.9, 0.15 };

// How far a woody plant's top is held over downwind where the gusts' field is `field`, swayed
// `seconds` in, as a share of its height, where the wind blows `speed` on the whole. Rocking at
// the same rate everywhere, as grass sways, so the gusts' field sets plants side by side apart
// smoothly and waves run through them; `apart` sets each a little apart from its neighbours.
float woody_sway(Woody wood, float seconds, float speed, float field, float apart) {
    const float gusted = gusting(speed, gust_of(field));
    const float held = (gusted * gusted) / ((gusted * gusted) + wood.holds);
    const float phase = (seconds * (wood.rate + (wood.quickening * speed))) + (6.0 * field) + apart;
    return (held * wood.lean) + (wood.rock * saturate(gusted / wood.rocks_from) * sin(phase));
}

public float bush_sway(float seconds, float speed, float field, float apart) {
    return woody_sway(bush_wood, seconds, speed, field, apart);
}

// For the trees drawn one by one and the grown forest alike, so neither moves as one gives way to
// the other.
public float tree_sway(float seconds, float speed, float field, float apart) {
    return woody_sway(tree_wood, seconds, speed, field, apart);
}
