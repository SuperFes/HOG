import atmosphere;
import bend;
import shadow;
import tone;
import water;

struct Push {
    float4x4 clip_from_eye;
    // xyz the water's origin from the eye, in metres; w seconds into the flow's cycles.
    float4 origin_seconds;
    // As bend.slang takes them: xyz up through the eye, w its height above sea level.
    float4 up_height;
    // xyz towards the sun; w the planet's radius.
    float4 sun_radius;
    // x the faked radius drawing bends the planet onto, y the radians across a pixel.
    float4 faked;
};

[[vk::push_constant]] ConstantBuffer<Push> push;

// When lit through the air.
[[vk::binding(0, 0)]] ConstantBuffer<Air> air;
[[vk::binding(1, 0)]] ConstantBuffer<Lighting> lighting;
[[vk::binding(2, 0)]] Sampler2D transmittance_table;
[[vk::binding(3, 0)]] Sampler2D sky_view_table;
[[vk::binding(4, 0)]] Sampler2DArray aerial_volume;
// As cloud_march.slang's.
[[vk::binding(5, 0)]] Sampler2D cloud_shadow;
// What's drawn before the water, as copied for it to be seen through.
[[vk::binding(6, 0)]] Sampler2D behind_colour;
[[vk::binding(7, 0)]] Sampler2D behind_depth;

// As hog::terrain::RiverVertex has it.
struct Vertex {
    [[vk::location(0)]] float3 position;
    [[vk::location(1)]] float beyond;
    [[vk::location(2)]] float3 flow;
    [[vk::location(3)]] float rough;
    [[vk::location(4)]] float3 outward;
    [[vk::location(5)]] float half;
};

struct Surface {
    float4 position : SV_Position;
    float3 from_eye;
    // Where it lies from the water's origin, for a pattern fixed to the ground.
    float3 placed;
    // Up from the planet's centre, as it truly lies.
    float3 up;
    float3 flow;
    float rough;
    // Metres past the water's nearer edge, and the metres a pixel spans there.
    float beyond;
    float pixel;
    // How much of the width it's drawn the water truly covers.
    float coverage;
};

// What rivers carry, as hog::terrain::river_carried has it: the dark of peaty water and a little
// silt.
static const float4 river_carried = float4(0.6, 3.0, 0.5, 0.0);

// As hog::terrain::river_pattern: the pattern repeats over this many metres along each axis.
static const float pattern = 256.0;
// Ripples carried on the flow, each over so many metres, which the pattern's repeat divides, and
// how steep each makes the surface.
static const float3 ripple_sizes = float3(0.5, 1.0, 4.0);
static const float3 ripple_slopes = float3(0.06, 0.05, 0.03);
// The flow carries the ripples so many seconds before they're laid anew, half a cycle apart.
static const float carried_for = 1.6;
// The mean square slope of ripples too small to draw.
static const float unseen_slope = 0.003;
// Of the light falling on white water, the share it scatters back.
static const float white_albedo = 0.5;
// Drawn this much nearer the eye than it lies, in metres and in what pixels span there: as far as
// ground drawn coarsely may stand over the water in its channel.
static const float nearer = 0.2;
static const float nearer_pixels = 3.0;
// Shown this far over its banks, at least, and so many pixels: as far as the ground drawn coarsely
// may leave the channel too narrow to show the water drawn nearer.
static const float shown_over = 0.1;
static const float shown_pixels = 0.75;
// Half as many pixels wide as it's drawn at least, so a river too far to span them stays a line
// unbroken from pixel to pixel, showing what's behind it through what it doesn't cover.
static const float least_pixels = 0.75;

[shader("vertex")]
Surface vertex_main(Vertex vertex) {
    Surface surface;
    const float3 from_eye = vertex.position + push.origin_seconds.xyz;
    const float pixel =
        length(bent(from_eye, push.up_height, float2(push.sun_radius.w, push.faked.x))) *
        push.faked.y;
    const float shown = max(vertex.half, least_pixels * pixel);
    const float3 widened = vertex.outward * (shown - vertex.half);
    surface.coverage = vertex.half / max(shown, 1e-6);
    surface.from_eye = from_eye + widened;
    surface.placed = vertex.position + widened;
    surface.up = normalize(surface.from_eye +
                           (push.up_height.xyz * (push.sun_radius.w + push.up_height.w)));
    surface.flow = vertex.flow;
    surface.rough = vertex.rough;
    surface.beyond = vertex.beyond;
    const float3 drawn = bent(surface.from_eye, push.up_height, float2(push.sun_radius.w, push.faked.x));
    const float away = length(drawn);
    surface.pixel = away * push.faked.y;
    // Along the line of sight, so it's drawn where it is on the picture.
    const float drawn_nearer = min(nearer + (nearer_pixels * surface.pixel), 0.5 * away);
    surface.position = mul(push.clip_from_eye, float4(drawn * (1.0 - (drawn_nearer / away)), 1.0));
    return surface;
}

uint hashed(uint3 at) {
    uint h = (at.x * 0x8da6b343u) ^ (at.y * 0xd8163841u) ^ (at.z * 0xcb1ab31fu);
    h ^= h >> 15;
    h *= 0x2c1b3c6du;
    h ^= h >> 12;
    return h;
}

// Of value noise from -1 to 1 over cells `cells` to the pattern, at `at` in cells: its value in w,
// and how fast it changes in xyz.
float4 noise_at(float3 at, uint cells) {
    const float3 cell = floor(at);
    const float3 f = at - cell;
    const float3 eased = f * f * f * ((f * ((f * 6.0) - 15.0)) + 10.0);
    const float3 easing = 30.0 * f * f * ((f * (f - 2.0)) + 1.0);
    float corner[8];
    for (uint k = 0; k < 8; ++k) {
        const int3 offset = int3(k & 1, (k >> 1) & 1, (k >> 2) & 1);
        const int3 wrapped = (int3(cell) + offset) % int(cells);
        const uint3 lattice = uint3(wrapped + select(wrapped < 0, int3(int(cells)), int3(0)));
        corner[k] = (float(hashed(lattice) & 0xffffu) / 32767.5) - 1.0;
    }
    const float x00 = lerp(corner[0], corner[1], eased.x);
    const float x10 = lerp(corner[2], corner[3], eased.x);
    const float x01 = lerp(corner[4], corner[5], eased.x);
    const float x11 = lerp(corner[6], corner[7], eased.x);
    const float y0 = lerp(x00, x10, eased.y);
    const float y1 = lerp(x01, x11, eased.y);
    const float value = lerp(y0, y1, eased.z);
    const float dx = easing.x * lerp(lerp(corner[1] - corner[0], corner[3] - corner[2], eased.y),
                                     lerp(corner[5] - corner[4], corner[7] - corner[6], eased.y),
                                     eased.z);
    const float dy = easing.y * lerp(x10 - x00, x11 - x01, eased.z);
    const float dz = easing.z * (y1 - y0);
    return float4(dx, dy, dz, value);
}

// How much of ripples `size` metres long a pixel spanning `footprint` metres shows.
float ripple_seen(float size, float footprint) {
    return saturate((size / max(footprint, 1e-3)) - 1.0);
}

// The mean square slope of the ripples too fine to show over `footprint` metres.
float ripples_unseen(float footprint, float strength) {
    float unseen = 0.0;
    for (uint k = 0; k < 3; ++k) {
        const float slope = ripple_slopes[k] * strength * (1.0 - ripple_seen(ripple_sizes[k], footprint));
        unseen += slope * slope;
    }
    return unseen;
}

// How steeply ripples carried `carried` metres on the flow slope the surface at `placed`, by the
// metre along each axis, as seen over `footprint` metres: ripples finer than that left out.
float3 ripples_at(float3 placed, float3 carried, float footprint, float strength) {
    float3 slope = float3(0.0);
    for (uint k = 0; k < 3; ++k) {
        const float size = ripple_sizes[k];
        const float seen = ripple_seen(size, footprint);
        if (seen <= 0.0) {
            continue;
        }
        const uint cells = uint(pattern / size);
        const float4 noise = noise_at((placed - carried) / size, cells);
        slope += noise.xyz * (ripple_slopes[k] * strength * seen);
    }
    return slope;
}

// How the surface faces at a pixel, the mean square slope of what's too small to draw there, and
// how much of it is white.
struct Facing {
    float3 normal;
    float roughness;
    float white;
};

Facing facing_at(Surface surface, float3 looking) {
    const float3 up = surface.up;
    const float speed = length(surface.flow);
    // Metres of the surface a pixel spans.
    const float footprint = length(fwidth(surface.placed));
    // Laid anew twice a cycle, carried on the flow between, each faded out as the other's laid.
    const float cycle = push.origin_seconds.w / carried_for;
    const float first = frac(cycle);
    const float second = frac(cycle + 0.5);
    const float strength = 0.6 + speed + (2.0 * surface.rough);
    const float3 slope =
        (ripples_at(surface.placed, surface.flow * (first * carried_for), footprint, strength) *
         (1.0 - abs((2.0 * first) - 1.0))) +
        (ripples_at(surface.placed + float3(37.0, 11.0, 23.0), surface.flow * (second * carried_for),
                    footprint, strength) *
         (1.0 - abs((2.0 * second) - 1.0)));
    Facing facing;
    facing.normal = normalize(up - (slope - (up * dot(slope, up))));
    facing.roughness = unseen_slope + ripples_unseen(footprint, strength);
    if (dot(facing.normal, -looking) < 0.0) {
        facing.normal = up;
    }
    // Rough water breaks white, in patches carried on the flow.
    const float4 patches =
        noise_at((surface.placed - (surface.flow * push.origin_seconds.w)) / 4.0, uint(pattern / 4.0));
    facing.white = saturate((surface.rough - 0.35) * 2.0) * saturate(0.5 + patches.w);
    return facing;
}

// The light falling on the water at the surface, over pi.
float3 on_water_at(Shade shade, float3 up) {
    const float3 sun = push.sun_radius.xyz;
    const float facing = saturate(dot(up, sun));
    const float3 clear = transmittance(air, transmittance_table, 0.0, dot(up, sun));
    return ((through_cloud(shade, clear) * facing) +
            sky_under_cloud(lighting, shade, clear * facing)) /
           pi;
}

// Of the surface seen through the air: the sky it reflects, the glint, and what's seen through it
// of what's drawn beneath, as copied, through the water between; and white water over that.
[shader("fragment")]
float4 fragment_through(Surface surface) : SV_Target {
    if (surface.beyond > max(shown_over, shown_pixels * surface.pixel)) {
        discard;
    }
    const float3 looking = normalize(surface.from_eye);
    const Facing facing = facing_at(surface, looking);
    const float3 sun = push.sun_radius.xyz;
    const float3 up = surface.up;
    const float exposure = lighting.sun_exposure.w;
    const Shade shade = shade_at(lighting, cloud_shadow, surface.from_eye);
    const float reflected = reflected_at(looking, facing.normal);
    const float3 sky = sky_mirrored(air, lighting, sky_view_table, transmittance_table, shade,
                                    reflect(looking, facing.normal), up, sun);
    const float3 sunlight = through_cloud(shade, transmittance(air, transmittance_table, 0.0,
                                                               dot(facing.normal, sun)));
    const float3 glint =
        sunlight * reflected * glint_lobe(looking, facing.normal, facing.roughness, sun);
    const float3 surface_drawn =
        bent(surface.from_eye, push.up_height, float2(push.sun_radius.w, push.faked.x));
    const float4 clip = mul(push.clip_from_eye, float4(surface_drawn, 1.0));
    const float2 ndc = clip.xy / clip.w;
    const float4 between = aerial_perspective(aerial_volume, (ndc * 0.5) + 0.5,
                                              length(surface.from_eye) * lighting.sky_scale.w,
                                              lighting.reach.x);
    const float3 on_water = on_water_at(shade, up);

    const int3 pixel = int3(int2(surface.position.xy), 0);
    const float depth = behind_depth.Load(pixel).x;
    float3 water = optics(river_carried).colour * on_water;
    if (depth > 0.0) {
        const float4 seen = mul(lighting.eye_from_clip, float4(ndc, depth, 1.0));
        const float3 behind = seen.xyz / seen.w;
        const float3 drawn = max((untoned(behind_colour.Load(pixel).rgb, exposure) - between.rgb) /
                                     max(between.a, 1e-4),
                                 0.0);
        const float under = max(dot(surface_drawn - behind, up), 0.0);
        water = through_water(drawn, on_water, river_carried, under, 1.0, length(behind),
                              dot(-looking, up));
    }
    const float3 radiance =
        between.rgb + (between.a * ((reflected * sky) + glint + ((1.0 - reflected) * water)));
    const float3 white = (white_albedo * ((sunlight * saturate(dot(facing.normal, sun))) +
                                          (on_water * pi)) *
                          between.a / pi) +
                         between.rgb;
    const float3 seen = tone_mapped(lerp(radiance, white, facing.white), exposure);
    return float4(lerp(behind_colour.Load(pixel).rgb, seen, surface.coverage), 1.0);
}
