import atmosphere;
import bend;
import shadow;
import shore;
import tone;
import water;
import waves;

struct Push {
    float4x4 clip_from_eye;
    // xyz up through the eye, w its height above sea level.
    float4 up_height;
    // xyz east along the ground under the eye, w the faked radius drawing bends the planet onto.
    float4 east_faked;
    // xyz towards the sun, w the metres the disc's rings start apart.
    float4 sun_nearest;
    // rgb the sky reflected, w the metres along the ground to the disc's rim.
    float4 sky_furthest;
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
// Lit through the air or not.
[[vk::binding(6, 0)]] ConstantBuffer<Waves> waves;
// As shore_waters and shore_fetch take them.
[[vk::binding(7, 0)]] Sampler2DArray shore_depths;
[[vk::binding(8, 0)]] Sampler2DArray shore_fetches;
// As hog::render::FoamMap keeps it.
[[vk::binding(9, 0)]] Sampler2D foam_kept;
// As hog::render::RippleMap keeps them.
[[vk::binding(10, 0)]] Sampler2D ripples_kept;
// As hog::render::RainTile draws it.
[[vk::binding(11, 0)]] Sampler2DArray<float2> rain_tile;
// What's drawn before the sea, as copied for it to lay the water over, when lit through the air.
[[vk::binding(12, 0)]] Sampler2D behind_colour;
[[vk::binding(13, 0)]] Sampler2D behind_depth;

struct Vertex {
    [[vk::location(0)]] float out; // from 0 under the eye to 1 at the rim
    [[vk::location(1)]] float2 towards; // east and north
};

struct Surface {
    float4 position : SV_Position;
    float3 from_eye;
    // Sea level's up, as it's drawn there.
    float3 normal;
    // The water's place at rest, at sea level from the eye as it truly lies.
    float3 rest;
    // The coarse waves of the foam's pattern there: as FoamPattern's coarse, across, coarse_seen
    // and coarse_slope.
    float4 foam;
    // The sea's floor there, as wave_surface takes it; w how high the sea stands over its mean.
    float4 floor;
    // Over still water, metres of it upwind; none over the sea.
    float fetch;
    // The surf there, as Surf's breaking, trailing, lace and white.
    float4 surf;
};

Surf as_surf(float4 surf) {
    Surf taken;
    taken.breaking = surf.x;
    taken.trailing = surf.y;
    taken.lace = surf.z;
    taken.white = surf.w;
    return taken;
}

// As ocean.cpp makes the disc.
static const float rings = 96.0;
static const float spokes = 128.0;

// The sun's angular radius, as sky.slang draws it. The stillest water mirrors its disc, whose
// halfway vectors spread over half that: the glint's least spread, as a mean square slope.
static const float sun_angular_radius = 0.004675;
static const float sun_spread = 0.25 * sun_angular_radius * sun_angular_radius;
// The mean square slope of ripples shorter than any wave, per m/s of wind: none in a calm.
static const float ripples_per_wind = 0.0002;
// Without the air, the glint off water this rough, as a mean square slope, peaks this bright.
static const float glint_reference = 0.001;
static const float glint_brightness = 50.0;
static const float stillest_lobe = ((2.0 / glint_reference) + 6.0) / (8.0 * pi);
// The water's surface too fine to draw is taken as flat facets this many metres across, each
// mirroring the sun or not: a pixel over few sparkles, one over many shows them as the lobe.
static const float facet = 0.02;
// Each facet curves, mirroring the sun over normals this many radians round its own.
static const float facet_bend = 0.05;
// Each pixel's facets turn anew about this often, seconds, as ripples pass.
static const float sparkle_life = 0.06;
// Pixels whose sparkles would vary by less than this share of their mean show the lobe.
static const float sparkles_unseen = 0.05;
// Of the light falling on foam fresh from a breaking crest, the share it scatters back; and on the
// thinner foam it leaves.
static const float breaking_albedo = 0.5;
static const float left_albedo = 0.3;
static const float left_thickness = 0.7;
// How thick breaking foam is on the whole.
static const float breaking_thickness = 0.8;
// Water shallower than this, metres, foams at the swash's edge.
static const float swash_foam = 0.12;
// Less foam kept than this is too little to see.
static const float least_kept = 0.004;

// Turns `v` as sea level's up under the eye, `truly`, turns to as it's drawn, `drawn`.
float3 as_drawn(float3 v, float3 truly, float3 drawn) {
    const float3 axis = cross(truly, drawn);
    const float facing = dot(truly, drawn);
    return (v * facing) + cross(axis, v) + (axis * (dot(axis, v) / (1.0 + facing)));
}

// As hog::terrain::sea_along and sea_from_eye place it, moved by the waves.
[shader("vertex")]
Surface vertex_main(Vertex vertex) {
    const float3 up = push.up_height.xyz;
    const float3 east = push.east_faked.xyz;
    const float faked = push.east_faked.w;
    const float nearest = push.sun_nearest.w;
    const float spread = (push.sky_furthest.w / nearest) + 1.0;
    const float along = nearest * (pow(spread, vertex.out) - 1.0);

    const float3 towards = (east * vertex.towards.x) + (cross(up, east) * vertex.towards.y);
    // Turned through angles small enough that the GPU's own sines would err by metres.
    const float turned = along / faked;
    const float half = small_sin(turned / 2.0);
    const float turned_sin = small_sin(turned);
    Surface surface;
    surface.from_eye =
        (up * ((-2.0 * faked * half * half) - push.up_height.w)) + (towards * (faked * turned_sin));
    surface.normal = (up * (1.0 - (2.0 * half * half))) + (towards * turned_sin);

    const float radius = waves.count_left_radius.z;
    const float truly = along / radius;
    const float truly_half = small_sin(truly / 2.0);
    surface.rest = (up * ((-2.0 * radius * truly_half * truly_half) - push.up_height.w)) +
                   (towards * (radius * small_sin(truly)));
    // The corners lie apart by the larger of the rings' spacing and the spokes'.
    const float between =
        max(nearest * log(spread) * pow(spread, vertex.out) / rings, along * 2.0 * pi / spokes);
    surface.floor = floor_at(waves, shore_depths, shore_fetches, surface.rest, surface.fetch);
    const WaveSurface water =
        wave_surface(waves, surface.rest, between, surface.floor.xyz, surface.fetch);
    surface.from_eye += as_drawn(water.moved + (up * surface.floor.w), up, surface.normal);
    // Still water's surface kept out under the ground round it breaks on nothing, as over the deep;
    // left to break there, it would whiten the water it's drawn across to.
    const bool kept_under = surface.fetch > 0.0 && surface.floor.x <= 0.0;
    const Surf surf = surf_at(waves, water, kept_under ? deep_floor() : surface.floor.xyz);
    surface.surf = float4(surf.breaking, surf.trailing, surf.lace, surf.white);
    surface.foam = float4(0.0);
    if (water.white.z > 0.0 || surf.white > 0.0) {
        const FoamPattern coarse = coarse_foam_at(waves, surface.rest, between);
        surface.foam = float4(coarse.coarse, coarse.across, coarse.coarse_seen, coarse.coarse_slope);
    }
    surface.position = mul(push.clip_from_eye, float4(surface.from_eye, 1.0));
    return surface;
}

// How the water's surface faces at a pixel, the mean square slope of what's too small to draw
// there, and how much of it is white with foam, and how bright that is.
struct Facing {
    float3 normal;
    float roughness;
    float white;
    float albedo;
    // Metres of the surface a pixel spans, and the mean square slope of the halfway vectors to
    // the ways it looks over, which spreads the glint over the pixel and no further.
    float footprint;
    float seen;
};

// As hog::render::ripple_size and ripple_spacing.
static const float ripple_size = 512.0;
static const float ripple_spacing = 0.125;
// Ripples fade out over so many texels in from the grid's edge, where they're mostly soaked up.
static const float ripple_fading = 32.0;
// Ripples are drawn where their texels span a pixel; seen over more than so many texels, they
// only spread the glint.
static const float ripples_unseen = 4.0;

// How steeply the ripples kept at `rest`, a point of sea level from the eye, slope along the
// grid's rows and columns, as drawn seen over `footprint` metres; and in `unseen`, the mean square
// slope of them too fine to draw. None without a grid.
float2 ripple_slope_at(float3 rest, float footprint, out float unseen) {
    unseen = 0.0;
    const float per_metre = waves.ripple_anchor.w;
    if (per_metre <= 0.0) {
        return float2(0.0);
    }
    const float3 from_anchor = rest - waves.ripple_anchor.xyz;
    const float2 texel = (float2(dot(from_anchor, waves.ripple_east.xyz),
                                 dot(from_anchor, waves.ripple_north.xyz)) -
                          float2(waves.ripple_east.w, waves.ripple_north.w)) *
                         per_metre;
    const float2 inside = min(texel, ripple_size - texel);
    const float fading = saturate(min(inside.x, inside.y) / ripple_fading);
    if (fading <= 0.0) {
        return float2(0.0);
    }
    const float2 at = texel / ripple_size;
    const float across = 1.0 / ripple_size;
    const float2 slope =
        float2(ripples_kept.SampleLevel(at + float2(across, 0.0), 0.0).x -
                   ripples_kept.SampleLevel(at - float2(across, 0.0), 0.0).x,
               ripples_kept.SampleLevel(at + float2(0.0, across), 0.0).x -
                   ripples_kept.SampleLevel(at - float2(0.0, across), 0.0).x) /
        (2.0 * ripple_spacing) * fading;
    const float seen =
        saturate((ripples_unseen - (footprint / ripple_spacing)) / (ripples_unseen - 1.0));
    unseen = dot(slope, slope) * (1.0 - seen);
    return slope * seen;
}

// As hog::render::rain_ring_cell and rain_tile_cells.
static const float rain_ring_cell = 0.3;
static const float rain_tile_cells = 8.0;
// The rings' ripples so many metres long.
static const float ring_length = 0.03;
// The mean square slope the most rain rings lend the sea where they're too fine to draw.
static const float rain_roughness = 0.02;
// The tile's second layer is laid turned by 0.9 radians, a turn that never brings a square back
// onto itself, and moved over by so many tiles, so the two never repeat together.
static const float2 tile_turning = float2(0.62161, 0.78333);
static const float2 tile_moved = float2(0.37, 0.71);

// How steeply the rain's rings slope the water at `rest`, a point of sea level from the eye, along
// the ripples' grid's rows and columns, as drawn seen over `footprint` metres; and in `unseen`,
// the mean square slope of them too fine to draw. None without a grid to lay them on.
float2 rain_slope_at(float3 rest, float footprint, out float unseen) {
    const float chance = waves.rain.x;
    // Drawn only as fine as the pixels can show their ripples.
    const float seen = saturate(2.0 - (2.0 * footprint / ring_length));
    unseen = rain_roughness * chance * (1.0 - seen);
    if (chance <= 0.0 || seen <= 0.0 || waves.ripple_anchor.w <= 0.0) {
        unseen = rain_roughness * chance;
        return float2(0.0);
    }
    const float3 from_anchor = rest - waves.ripple_anchor.xyz;
    const float2 along =
        float2(dot(from_anchor, waves.ripple_east.xyz), dot(from_anchor, waves.ripple_north.xyz)) /
        (rain_ring_cell * rain_tile_cells);
    const float2 turned = float2((tile_turning.x * along.x) - (tile_turning.y * along.y),
                                 (tile_turning.y * along.x) + (tile_turning.x * along.y)) +
                          tile_moved;
    const float2 first = rain_tile.SampleLevel(float3(along, 0.0), 0.0);
    // Sloping along the turned layer's own rows and columns, turned back onto the grid's.
    const float2 second = rain_tile.SampleLevel(float3(turned, 1.0), 0.0);
    return (first + float2((tile_turning.x * second.x) + (tile_turning.y * second.y),
                           (tile_turning.x * second.y) - (tile_turning.y * second.x))) *
           seen;
}

// How deep the sea stands at `rest`, as floor_at finds it, but at each pixel rather than between
// the disc's corners, so what's drawn by it follows the shore rather than the disc.
float sea_depth_at(float3 rest) {
    const float3 floor = shore_floor(waves, shore_depths, rest);
    return floor.x + level_now(shore_level_at(waves, floor, rest));
}

// How much the swash's edge foams: as much as the waves running up to it are high, beside how deep
// it foams over.
float swashing() {
    return saturate(running_up(waves) / swash_foam);
}

// Whether the surface is a lake's kept out over ground the grids find dry, which isn't drawn: the
// ground would hide it, but where the level runs out over lower ground between their texels, or
// waves lift it up the shore between the disc's corners far off. Told at each pixel. The ground's
// shading tints ground under the lake as far as its own height tells it, so no water is lost.
bool over_dry(Surface surface) {
    return surface.fetch > 0.0 && surface.floor.w > 0.0 &&
           still_water_at(waves, shore_depths, surface.rest).y <= 0.0;
}

// From `beneath`, its underside is what faces the eye.
Facing facing_at(Surface surface, float3 looking, bool beneath = false) {
    const float footprint = max(length(ddx(surface.rest)), length(ddy(surface.rest)));
    const WaveSurface water =
        wave_surface(waves, surface.rest, footprint, surface.floor.xyz, surface.fetch);
    float unseen;
    float unseen_rain;
    const float2 slope = ripple_slope_at(surface.rest, footprint, unseen) +
                         rain_slope_at(surface.rest, footprint, unseen_rain);
    const float3 rippled = normalize(water.normal - (waves.ripple_east.xyz * slope.x) -
                                     (waves.ripple_north.xyz * slope.y));
    Facing facing;
    facing.normal = as_drawn(rippled, push.up_height.xyz, normalize(surface.normal));
    facing.roughness = water.roughness + sun_spread +
                       (ripples_per_wind * waves.still_growth.z) + unseen + unseen_rain;
    facing.footprint = footprint;
    facing.seen = 0.25 * max(dot(ddx(looking), ddx(looking)), dot(ddy(looking), ddy(looking)));
    facing.white = 0.0;
    facing.albedo = breaking_albedo;
    const Surf surf = as_surf(surface.surf);
    const float2 kept = kept_foam_at(waves, foam_kept, surface.rest).xy;
    const bool any_kept = kept.x > least_kept;
    if (water.white.z > 0.0 || surf.white > 0.0 || any_kept) {
        FoamPattern coarse;
        coarse.coarse = surface.foam.x;
        coarse.across = surface.foam.y;
        coarse.coarse_seen = surface.foam.z;
        coarse.coarse_slope = surface.foam.w;
        coarse.fine = 0.0;
        coarse.fine_seen = 0.0;
        coarse.fine_across = 0.0;
        coarse.fine_slope = 0.0;
        const FoamPattern pattern =
            foam_pattern_at(waves, water, surf, coarse, surface.rest, footprint, any_kept);
        const Foam foam = foam_at(waves, water, surf, pattern, footprint);
        // Where it's seen, thickest well over where they break and thin where they barely do;
        // what's left is thinner still.
        const float seen = (pattern.coarse_seen + pattern.fine_seen) / 2.0;
        const float thickness =
            lerp(breaking_thickness,
                 saturate(0.5 + foam.over + (0.35 * pattern.fine)), seen);
        // The swash's edge runs foaming up the shore, ragged with the pattern's lace, as much as
        // the waves running up it are high. Still water has no swash.
        const float edge = (surface.fetch > 0.0 ? 0.0 : 1.0) * swashing() *
                           (1.0 - smoothstep(0.0, swash_foam, sea_depth_at(surface.rest))) *
                           saturate(2.0 * (pattern.fine + pattern.across + 0.3));
        const float breaking = max(foam.breaking * thickness, edge * breaking_thickness);
        // Foam left thins as it ages.
        const float left = left_with_kept(foam, pattern, kept, footprint) *
                           min(left_thickness * (0.4 + (1.2 * foam.fresh)), 1.0) * (1.0 - breaking);
        facing.white = breaking + left;
        facing.albedo = facing.white > 0.0
                            ? ((breaking_albedo * breaking) + (left_albedo * left)) / facing.white
                            : breaking_albedo;
    }
    // A wave's back, which the eye could only see past its crest, is taken as seen edge on.
    const float3 to_eye = beneath ? looking : -looking;
    const float towards = dot(facing.normal, to_eye);
    if (towards < 0.0) {
        facing.normal = normalize(facing.normal - (to_eye * towards));
    }
    return facing;
}

uint hashed(uint at) {
    at ^= at >> 16;
    at *= 0x7feb352du;
    at ^= at >> 15;
    at *= 0x846ca68bu;
    at ^= at >> 16;
    return at;
}

float unit(uint at) {
    return (float(hashed(at) >> 8) + 0.5) / 16777216.0;
}

// Of the glint at `pixel`, spanning `footprint` metres, the share its facets catching the sun now
// show: each facet's normal lies within the sun's disc, halfway to the eye, by the chance `lobe`
// gives, the surface's own, so a pixel shows as many as are dealt it of those it spans.
float sparkling(float lobe, float footprint, uint2 pixel) {
    // Over a pixel or less of a facet, neighbouring pixels glint together, as the lobe has it.
    const float facets = (footprint * footprint) / (facet * facet);
    if (facets <= 1.0) {
        return 1.0;
    }
    const float caught =
        min(lobe * pi * ((sun_angular_radius * sun_angular_radius) + (facet_bend * facet_bend)), 1.0);
    const float expected = facets * caught;
    const float spread = expected * (1.0 - caught);
    if (expected <= 0.0 || spread <= sparkles_unseen * sparkles_unseen * expected * expected) {
        return 1.0;
    }
    // Each pixel turns at its own moments, so the sparkles don't all change together.
    const uint seed = hashed(pixel.x ^ hashed(pixel.y + 0x9e3779b9u));
    const uint turn = uint(floor((waves.rain.y / sparkle_life) + unit(seed)));
    const uint dealt = hashed(seed ^ (turn * 0x85ebca6bu));
    float shown;
    if (expected < 1.0) {
        shown = unit(dealt) < expected ? 1.0 / expected : 0.0;
    } else {
        // Box and Muller's normal deviate, as a binomial count this many tends to.
        const float normal = sqrt(-2.0 * log(unit(dealt))) * cos(2.0 * pi * unit(dealt + 1u));
        shown = max(round(expected + (sqrt(spread) * normal)), 0.0) / expected;
    }
    // Breaking up from a facet a pixel to a few.
    return lerp(1.0, shown, saturate(0.5 * log2(facets)));
}

// How much of the sun's light the glint reflects towards the eye at `pixel`, but for the
// reflection: a normalised Blinn-Phong lobe as wide as the surface is rough, broken into the
// sparkles of its facets.
float glint_at(float3 looking, Facing facing, float3 sun, uint2 pixel) {
    const float lobe = glint_lobe(looking, facing.normal, facing.roughness, sun);
    return glint_lobe(looking, facing.normal, facing.roughness + facing.seen, sun) *
           sparkling(lobe, facing.footprint, pixel);
}

[shader("fragment")]
float4 fragment_main(Surface surface) : SV_Target {
    if (over_dry(surface)) {
        discard;
    }
    const float3 looking = normalize(surface.from_eye);
    const Facing facing = facing_at(surface, looking);
    const float reflected = reflected_at(looking, facing.normal);
    const float glint =
        glint_brightness * reflected *
        glint_at(looking, facing, push.sun_nearest.xyz, uint2(surface.position.xy)) / stillest_lobe;
    // Premultiplied: the sky by how much is reflected, and the glint over all; and over that the
    // foam, lit by the sun and as much again by the sky.
    const float3 foam =
        facing.albedo * (saturate(dot(facing.normal, push.sun_nearest.xyz)) + push.sky_furthest.rgb);
    return lerp(float4((push.sky_furthest.rgb * reflected) + glint, reflected), float4(foam, 1.0),
                facing.white);
}

// Of the sea at a pixel lit through the air: how much of the sky it reflects and what of it, the
// glint, the light falling on its foam, and the air between it and the eye.
struct AirLit {
    float reflected;
    float3 sky;
    float3 glint;
    float3 on_foam;
    float4 between;
    Shade shade;
};

AirLit air_lit(Surface surface, Facing facing, float3 looking) {
    const float3 normal = facing.normal;
    const float3 sun = push.sun_nearest.xyz;
    const float3 up = push.up_height.xyz;
    const float sun_mu = dot(sun, up);
    AirLit lit;
    lit.reflected = reflected_at(looking, normal);
    lit.shade = shade_at(lighting, cloud_shadow, surface.from_eye);
    lit.sky = sky_mirrored(air, lighting, sky_view_table, transmittance_table, lit.shade,
                           reflect(looking, normal), up, sun);
    const float3 sunlight =
        through_cloud(lit.shade, transmittance(air, transmittance_table, 0.0, dot(normal, sun)));
    lit.glint =
        sunlight * lit.reflected * glint_at(looking, facing, sun, uint2(surface.position.xy));
    lit.on_foam = (sunlight * saturate(dot(normal, sun))) +
                  sky_under_cloud(lighting, lit.shade,
                                  transmittance(air, transmittance_table, 0.0, sun_mu) *
                                      max(sun_mu, 0.0));
    const float4 clip = mul(push.clip_from_eye, float4(surface.from_eye, 1.0));
    const float2 uv = ((clip.xy / clip.w) * 0.5) + 0.5;
    lit.between = aerial_perspective(aerial_volume, uv,
                                     length(surface.from_eye) * lighting.sky_scale.w,
                                     lighting.reach.x);
    return lit;
}

// Premultiplied: over what's beneath, which has the air between already, the reflection seen
// through the air, and the glint; and over that the foam, lit as the ground is.
float4 air_premultiplied(Facing facing, AirLit lit) {
    const float4 between = lit.between;
    const float3 radiance =
        (lit.reflected * ((lit.sky * between.a) + between.rgb)) + (lit.glint * between.a);
    const float3 foam = (facing.albedo * lit.on_foam * between.a / pi) + between.rgb;
    return float4(tone_mapped(lerp(radiance, foam, facing.white), lighting.sun_exposure.w),
                  lerp(lit.reflected, 1.0, facing.white));
}

// Reflecting the sky as the eye sees it, and the sun's light through the air, seen through the air
// between, and tone mapped.
[shader("fragment")]
float4 fragment_air(Surface surface) : SV_Target {
    if (over_dry(surface)) {
        discard;
    }
    const float3 looking = normalize(surface.from_eye);
    const Facing facing = facing_at(surface, looking);
    return air_premultiplied(facing, air_lit(surface, facing, looking));
}

// The light falling on the water under the surface facing `up`, over pi, as the ground's shading
// has it.
float3 on_water_at(Shade shade, float3 up) {
    const float3 sun = push.sun_nearest.xyz;
    const float facing = saturate(dot(up, sun));
    const float3 clear = transmittance(air, transmittance_table, 0.0, dot(up, sun));
    return ((through_cloud(shade, clear) * facing) +
            sky_under_cloud(lighting, shade, clear * facing)) /
           pi;
}

// What's drawn beneath the sea at a pixel, as copied: where, from the eye, and which.
struct Behind {
    float3 at;
    int3 pixel;
};

// At `ndc`, the picture's own coordinates, in `pixel`, `depth` as the copy keeps it.
Behind behind_at(float2 ndc, int3 pixel, float depth) {
    const float4 seen = mul(lighting.eye_from_clip, float4(ndc, depth, 1.0));
    Behind behind;
    behind.at = seen.xyz / seen.w;
    behind.pixel = pixel;
    return behind;
}

// What's seen beneath the surface in place of `straight` as the waves bend the line of sight,
// tilting the surface to `normal` from `level`; unless that's off the picture, or out of the
// water, where it would be seen through the surface from beneath.
Behind bent_behind(Surface surface, Behind straight, float3 looking, float3 normal, float3 level) {
    const float under = length(straight.at - surface.from_eye);
    const float3 moved = straight.at + bent_by_waves(looking, normal, level, under);
    const float4 clip = mul(push.clip_from_eye, float4(moved, 1.0));
    const float2 ndc = clip.xy / clip.w;
    if (clip.w <= 0.0 || any(abs(ndc) >= 1.0)) {
        return straight;
    }
    uint width;
    uint height;
    behind_depth.GetDimensions(width, height);
    const int3 pixel = int3(int2(((ndc * 0.5) + 0.5) * float2(width, height)), 0);
    const float depth = behind_depth.Load(pixel).x;
    if (depth <= 0.0) {
        return straight;
    }
    const Behind bent = behind_at(ndc, pixel, depth);
    return dot(surface.from_eye - bent.at, level) > 0.0 ? bent : straight;
}

// The radius drawing bends the planet onto.
float faked_radius() {
    return push.east_faked.w;
}

// What's seen out of the water from `surface` along `out`: what's drawn there, as copied, where it
// stands out of the water, as near as the picture tells, found first as if far off and then as far
// as what's drawn there; otherwise the sky.
float3 out_of_water(Surface surface, float3 out, Shade shade) {
    const float exposure = lighting.sun_exposure.w;
    const float3 sky = sky_mirrored(air, lighting, sky_view_table, transmittance_table, shade, out,
                                    push.up_height.xyz, push.sun_nearest.xyz);
    uint width;
    uint height;
    behind_depth.GetDimensions(width, height);
    float4 clip = mul(push.clip_from_eye, float4(out, 0.0));
    int3 pixel = int3(0);
    for (int step = 0; step < 2; ++step) {
        const float2 ndc = clip.xy / clip.w;
        if (clip.w <= 0.0 || any(abs(ndc) >= 1.0)) {
            return sky;
        }
        pixel = int3(int2(((ndc * 0.5) + 0.5) * float2(width, height)), 0);
        const float depth = behind_depth.Load(pixel).x;
        if (depth <= 0.0) {
            break;
        }
        const float3 drawn = behind_at(ndc, pixel, depth).at;
        if (height_above_sea(drawn, push.up_height, faked_radius()) <= 0.0) {
            return sky;
        }
        clip = mul(push.clip_from_eye,
                   float4(surface.from_eye + (out * length(drawn - surface.from_eye)), 1.0));
    }
    return untoned(behind_colour.Load(pixel).rgb, exposure);
}

// Of the sea's surface seen from an eye in the water, the light coming down through it: out of
// the air within the window the water bends it all into, the rest turned back off the surface from
// the water beneath; under foam, what the foam lets through; and the water between it and the eye.
float4 from_beneath(Surface surface, float3 looking) {
    const Facing facing = facing_at(surface, looking, true);
    const float3 up = push.up_height.xyz;
    const float3 normal = facing.normal;
    const Shade shade = shade_at(lighting, cloud_shadow, surface.from_eye);
    const float3 on_water = on_water_at(shade, up);
    const Optics water = optics(lighting.carried);
    // None bends out past the window's edge, where the surface turns all of it back.
    const float3 out = refract(looking, -normal, 1.0 / into_water);
    float reflected = 1.0;
    float3 seen = float3(0.0);
    if (dot(out, out) > 0.0) {
        reflected = reflected_at(out, normal);
        // Crowded into the window as it bends, brighter by the square of how much.
        seen = (1.0 - reflected) * out_of_water(surface, out, shade) / (into_water * into_water);
    }
    // Turned back up off a wave's slope, it would meet the surface again, taken as seen level.
    const float3 down = reflect(looking, normal);
    seen += reflected *
            water_between(water, on_water, 0.0, max(-dot(down, up), 0.0), endless).scattered;
    seen = lerp(seen, on_water * (1.0 - facing.albedo), facing.white);
    // The water between, deepest at the eye, as far under this surface as it is.
    const float along = length(surface.from_eye);
    const WaterBetween between = water_between(water, on_water, max(dot(surface.from_eye, up), 0.0),
                                               -dot(looking, up), along);
    return float4(tone_mapped((seen * between.passed) + between.scattered,
                              lighting.sun_exposure.w),
                  1.0);
}

// As fragment_air, but over what's drawn beneath, as copied; where the sea lays the water itself,
// what lies behind the surface is dimmed and filled in by the water between, as deep under the
// surface drawn as it lies, and where nothing does, deep water. Seen from an eye in the water,
// as from_beneath has it.
[shader("fragment")]
float4 fragment_through(Surface surface) : SV_Target {
    if (over_dry(surface)) {
        discard;
    }
    const float3 looking = normalize(surface.from_eye);
    const int3 pixel = int3(int2(surface.position.xy), 0);
    if (sea_lays_water(lighting)) {
        const float4 clip = mul(push.clip_from_eye, float4(surface.from_eye, 1.0));
        if (in_water(lighting, behind_at(clip.xy / clip.w, pixel, 1.0).at)) {
            // Bent down below the eye by the planet's drawn curve, its top lies past where the line
            // of sight would have left the water; what's seen there is the water fragment_under
            // draws.
            if (dot(surface.from_eye, surface.normal) < 0.0) {
                discard;
            }
            return from_beneath(surface, looking);
        }
    }
    const Facing facing = facing_at(surface, looking);
    const AirLit lit = air_lit(surface, facing, looking);
    const float depth = behind_depth.Load(pixel).x;
    if (!sea_lays_water(lighting)) {
        // Where nothing's drawn beneath, from over the sea, the deep water fragment_deep draws.
        const float3 beneath = depth <= 0.0 && push.up_height.w > 0.0
                                   ? deep_water(surface)
                                   : behind_colour.Load(pixel).rgb;
        const float4 over = air_premultiplied(facing, lit);
        return float4(over.rgb + ((1.0 - over.a) * beneath), 1.0);
    }
    const float4 between = lit.between;
    const float exposure = lighting.sun_exposure.w;
    const float3 up = normalize(surface.normal);
    const float3 on_water = on_water_at(lit.shade, up);

    float3 water = optics(lighting.carried).colour * on_water;
    if (depth > 0.0) {
        const float4 clip = mul(push.clip_from_eye, float4(surface.from_eye, 1.0));
        const Behind straight = behind_at(clip.xy / clip.w, pixel, depth);
        const Behind bent = bent_behind(surface, straight, looking, facing.normal, up);
        const float3 behind = bent.at;
        // Drawn with the air between it and the eye, which the surface's stands in for.
        const float3 drawn =
            max((untoned(behind_colour.Load(bent.pixel).rgb, exposure) - between.rgb) /
                    max(between.a, 1e-4),
                0.0);
        // Seen from over the surface, though the eye may stand in a trough under the sea's level.
        water = through_water(drawn, on_water, lighting.carried, dot(surface.from_eye - behind, up),
                              max(push.up_height.w, 0.0), length(behind), dot(-looking, up));
    }
    const float3 radiance =
        between.rgb +
        (between.a * ((lit.reflected * lit.sky) + lit.glint + ((1.0 - lit.reflected) * water)));
    const float3 foam = (facing.albedo * lit.on_foam * between.a / pi) + between.rgb;
    return float4(tone_mapped(lerp(radiance, foam, facing.white), exposure), 1.0);
}

// The water under the surface as the ground's shading has it where it lies too deep to show: the
// light the water scatters back, lit facing up at sea level, seen through the air between, and
// tone mapped.
float3 deep_water(Surface surface) {
    const float3 up = normalize(surface.normal);
    const float3 sun = push.sun_nearest.xyz;
    const float scale = lighting.sky_scale.w;
    const float3 clear = transmittance(air, transmittance_table, 0.0, dot(up, sun));
    const Shade shade = shade_at(lighting, cloud_shadow, surface.from_eye);
    const float3 sunlight = through_cloud(shade, clear);
    const float3 sky = sky_under_cloud(lighting, shade, clear * saturate(dot(up, sun)));
    const float3 on_water = (sunlight * saturate(dot(up, sun))) + sky;
    const float3 radiance = optics(lighting.carried).colour * on_water / pi;

    const float4 clip = mul(push.clip_from_eye, float4(surface.from_eye, 1.0));
    const float2 uv = ((clip.xy / clip.w) * 0.5) + 0.5;
    const float4 between =
        aerial_perspective(aerial_volume, uv, length(surface.from_eye) * scale, lighting.reach.x);
    return tone_mapped((radiance * between.a) + between.rgb, lighting.sun_exposure.w);
}

// Drawn behind all else, so ground under the sea left undrawn looks as it would.
[shader("fragment")]
float4 fragment_deep(Surface surface) : SV_Target {
    return float4(deep_water(surface), 1.0);
}

// A triangle covering the whole picture at the near plane, where nothing drawn hides it; none
// where the eye stands too far out of the water for any of the near plane to be in it.
struct Screen {
    float4 position : SV_Position;
    float2 clip;
};

[shader("vertex")]
Screen vertex_screen(uint id: SV_VertexID) {
    const float2 clip = (float2((id << 1) & 2, id & 2) * 2.0) - 1.0;
    float reach = 0.0;
    for (int corner = 0; corner < 4; ++corner) {
        const float2 at = float2(corner & 1, corner >> 1) * 2.0 - 1.0;
        const float4 near = mul(lighting.eye_from_clip, float4(at, 1.0, 1.0));
        reach = max(reach, length(near.xyz / near.w));
    }
    Screen screen;
    screen.position = lighting.water.w > reach ? float4(0.0, 0.0, 1.0, 1.0) : float4(clip, 1.0, 1.0);
    screen.clip = clip;
    return screen;
}

// Farther than any light gets through the water.
static const float endless = 1.0e30;

// From an eye in the water, what's drawn in it, as copied, seen through the water between: dimmed
// by the water the light passes down to it and on to the eye, and filled in by what that water
// scatters of the light falling on the sea, less the deeper it's scattered; where nothing's drawn,
// the water alone. Drawn before the sea's surface, which hides it where it's seen from beneath.
[shader("fragment")]
float4 fragment_under(Screen screen) : SV_Target {
    const int3 pixel = int3(int2(screen.position.xy), 0);
    // Where the near plane is out of the water, the eye sees out of it.
    const float3 near = behind_at(screen.clip, pixel, 1.0).at;
    if (!in_water(lighting, near)) {
        discard;
    }
    const float3 up = push.up_height.xyz;
    const float exposure = lighting.sun_exposure.w;
    const float3 on_water = on_water_at(shade_at(lighting, cloud_shadow, float3(0.0)), up);
    const float deep = max(-lighting.water.w, 0.0);
    const float sinking = -dot(normalize(near), up);
    const float depth = behind_depth.Load(pixel).x;
    const float along = depth > 0.0 ? length(behind_at(screen.clip, pixel, depth).at) : endless;
    const Optics water = optics(lighting.carried);
    const WaterBetween between = water_between(water, on_water, deep, sinking, along);
    float3 seen = between.scattered;
    if (depth > 0.0) {
        const float3 drawn = untoned(behind_colour.Load(pixel).rgb, exposure);
        seen += drawn * between.passed *
                exp(-water.dimming * max(deep + (sinking * along), 0.0));
    }
    return float4(tone_mapped(seen, exposure), 1.0);
}
