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
// As shore_depth takes them.
[[vk::binding(7, 0)]] Sampler2DArray shore_depths;

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

// Metres over which the floor's slope is taken: half the finest depths' spacing, so it changes
// smoothly across them.
static const float floor_step = 4.0;

float depth_at(float3 rest) {
    return shore_depth(shore_depths, waves.shore_anchor, waves.shore_east, waves.shore_north,
                       waves.count_left_radius.z, rest);
}

// The sea's floor under `rest`, a point of sea level from the eye, with the water deepened by the
// sea standing over its mean there, as sea_level_at has it; and w how high it stands.
float4 floor_at(float3 rest) {
    const float depth = depth_at(rest);
    const float level = (waves.tide.w + dot(waves.tide.xyz, rest)) * sea_reached(depth);
    if (depth + level >= waves.count_left_radius.w) {
        return float4(deep_floor(), level);
    }
    const float3 first = waves.first.xyz * floor_step;
    const float3 second = waves.second.xyz * floor_step;
    return float4(depth + level,
                  (depth_at(rest + first) - depth_at(rest - first)) / (2.0 * floor_step),
                  (depth_at(rest + second) - depth_at(rest - second)) / (2.0 * floor_step), level);
}

// As ocean.cpp makes the disc.
static const float rings = 96.0;
static const float spokes = 128.0;

// The mean square slope of ripples shorter than any wave: the glint's least spread.
static const float ripples = 0.001;
// Without the air, the glint off the stillest water peaks this bright.
static const float glint_brightness = 50.0;
static const float stillest_lobe = ((2.0 / ripples) + 6.0) / (8.0 * pi);
// Of the light falling on foam fresh from a breaking crest, the share it scatters back; and on the
// thinner foam it leaves.
static const float breaking_albedo = 0.5;
static const float left_albedo = 0.3;
static const float left_thickness = 0.7;
// How thick breaking foam is on the whole.
static const float breaking_thickness = 0.8;

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
    surface.floor = floor_at(surface.rest);
    const WaveSurface water = wave_surface(waves, surface.rest, between, surface.floor.xyz);
    surface.from_eye += as_drawn(water.moved + (up * surface.floor.w), up, surface.normal);
    const Surf surf = surf_at(waves, water, surface.floor.xyz);
    surface.surf = float4(surf.breaking, surf.trailing, surf.lace, surf.white);
    surface.foam = float4(0.0);
    if (waves.foam.z > 0.0 || surf.white > 0.0) {
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
};

Facing facing_at(Surface surface, float3 looking) {
    const float footprint = max(length(ddx(surface.rest)), length(ddy(surface.rest)));
    const WaveSurface water = wave_surface(waves, surface.rest, footprint, surface.floor.xyz);
    Facing facing;
    facing.normal = as_drawn(water.normal, push.up_height.xyz, normalize(surface.normal));
    facing.roughness = water.roughness + ripples;
    facing.white = 0.0;
    facing.albedo = breaking_albedo;
    const Surf surf = as_surf(surface.surf);
    if (waves.foam.z > 0.0 || surf.white > 0.0) {
        FoamPattern coarse;
        coarse.coarse = surface.foam.x;
        coarse.across = surface.foam.y;
        coarse.coarse_seen = surface.foam.z;
        coarse.coarse_slope = surface.foam.w;
        coarse.fine = 0.0;
        coarse.fine_seen = 0.0;
        coarse.fine_slope = 0.0;
        const FoamPattern pattern =
            foam_pattern_at(waves, water, surf, coarse, surface.rest, footprint);
        const Foam foam = foam_at(waves, water, surf, pattern, footprint);
        // Where it's seen, thickest well over where they break and thin where they barely do;
        // what's left is thinner still.
        const float seen = (pattern.coarse_seen + pattern.fine_seen) / 2.0;
        const float thickness =
            lerp(breaking_thickness,
                 saturate(0.5 + foam.over + (0.35 * pattern.fine)), seen);
        const float breaking = foam.breaking * thickness;
        // Foam left thins as it ages.
        const float left = foam.left * min(left_thickness * (0.4 + (1.2 * foam.fresh)), 1.0) *
                           (1.0 - breaking);
        facing.white = breaking + left;
        facing.albedo = facing.white > 0.0
                            ? ((breaking_albedo * breaking) + (left_albedo * left)) / facing.white
                            : breaking_albedo;
    }
    // A wave's back, which the eye could only see past its crest, is taken as seen edge on.
    const float towards = dot(facing.normal, -looking);
    if (towards < 0.0) {
        facing.normal = normalize(facing.normal + (looking * towards));
    }
    return facing;
}

// How much of the sun's light the glint reflects towards the eye, but for the reflection: a
// normalised Blinn-Phong lobe as wide as the surface is rough.
float glint_at(float3 looking, Facing facing, float3 sun) {
    const float sharpness = (2.0 / facing.roughness) - 2.0;
    const float3 halfway = normalize(sun - looking);
    const float lit = saturate(dot(facing.normal, sun) * 10.0);
    return lit * ((sharpness + 8.0) / (8.0 * pi)) *
           pow(saturate(dot(facing.normal, halfway)), sharpness);
}

[shader("fragment")]
float4 fragment_main(Surface surface) : SV_Target {
    const float3 looking = normalize(surface.from_eye);
    const Facing facing = facing_at(surface, looking);
    const float reflected = reflected_at(looking, facing.normal);
    const float glint =
        glint_brightness * reflected * glint_at(looking, facing, push.sun_nearest.xyz) / stillest_lobe;
    // Premultiplied: the sky by how much is reflected, and the glint over all; and over that the
    // foam, lit by the sun and as much again by the sky.
    const float3 foam =
        facing.albedo * (saturate(dot(facing.normal, push.sun_nearest.xyz)) + push.sky_furthest.rgb);
    return lerp(float4((push.sky_furthest.rgb * reflected) + glint, reflected), float4(foam, 1.0),
                facing.white);
}

// Reflecting the sky as the eye sees it, and the sun's light through the air, seen through the air
// between, and tone mapped.
[shader("fragment")]
float4 fragment_air(Surface surface) : SV_Target {
    const float3 looking = normalize(surface.from_eye);
    const Facing facing = facing_at(surface, looking);
    const float3 normal = facing.normal;
    const float reflected = reflected_at(looking, normal);
    const float3 sun = push.sun_nearest.xyz;
    const float3 up = push.up_height.xyz;
    const float height = lighting.up_height.w;
    const float scale = lighting.sky_scale.w;

    // The reflected view, read from the eye's sky-view table as if seen from the eye.
    const float3 mirrored = reflect(looking, normal);
    // Reflected down, it meets another wave, as bright as the sky low down.
    const float mu = max(dot(mirrored, up), 0.0);
    const float sun_mu = dot(sun, up);
    const float3 flat_view = mirrored - (up * mu);
    const float3 flat_sun = sun - (up * sun_mu);
    const float across = length(flat_view) * length(flat_sun);
    const float cos_azimuth = across > 1e-6 ? dot(flat_view, flat_sun) / across : 1.0;
    const float3 clear_sky =
        sky_view_table.SampleLevel(sky_view_uv(air, height, mu, cos_azimuth), 0.0).rgb;
    // Where cloud hides the sky, the cloud's underside, as bright as the light it lets through.
    const Shade shade = shade_at(lighting, cloud_shadow, surface.from_eye);
    const float3 under =
        sky_under_cloud(lighting, shade,
                        transmittance(air, transmittance_table, 0.0, sun_mu) * max(sun_mu, 0.0)) /
        pi;
    const float3 sky = lerp(under, clear_sky, shade.open);
    const float3 sunlight =
        through_cloud(shade, transmittance(air, transmittance_table, 0.0, dot(normal, sun)));
    const float3 glint = sunlight * reflected * glint_at(looking, facing, sun);

    const float4 clip = mul(push.clip_from_eye, float4(surface.from_eye, 1.0));
    const float2 uv = ((clip.xy / clip.w) * 0.5) + 0.5;
    const float4 between =
        aerial_perspective(aerial_volume, uv, length(surface.from_eye) * scale, lighting.reach.x);
    // Premultiplied: over what's beneath, which has the air between already, the reflection seen
    // through the air, and the glint; and over that the foam, lit as the ground is.
    const float3 radiance = (reflected * ((sky * between.a) + between.rgb)) + (glint * between.a);
    const float3 on_foam = (sunlight * saturate(dot(normal, sun))) +
                           sky_under_cloud(lighting, shade,
                                           transmittance(air, transmittance_table, 0.0, sun_mu) *
                                               max(sun_mu, 0.0));
    const float3 foam = (facing.albedo * on_foam * between.a / pi) + between.rgb;
    return float4(tone_mapped(lerp(radiance, foam, facing.white), lighting.sun_exposure.w),
                  lerp(reflected, 1.0, facing.white));
}

// The water under the surface as the ground's shading has it where it lies too deep to show: the
// light the water scatters back, lit facing up at sea level, seen through the air between, and
// tone mapped. Drawn behind all else, so ground under the sea left undrawn looks as it would.
[shader("fragment")]
float4 fragment_deep(Surface surface) : SV_Target {
    const float3 up = normalize(surface.normal);
    const float3 sun = push.sun_nearest.xyz;
    const float scale = lighting.sky_scale.w;
    const float3 clear = transmittance(air, transmittance_table, 0.0, dot(up, sun));
    const Shade shade = shade_at(lighting, cloud_shadow, surface.from_eye);
    const float3 sunlight = through_cloud(shade, clear);
    const float3 sky = sky_under_cloud(lighting, shade, clear * saturate(dot(up, sun)));
    const float3 on_water = (sunlight * saturate(dot(up, sun))) + sky;
    const float3 radiance = water_colour * on_water / pi;

    const float4 clip = mul(push.clip_from_eye, float4(surface.from_eye, 1.0));
    const float2 uv = ((clip.xy / clip.w) * 0.5) + 0.5;
    const float4 between =
        aerial_perspective(aerial_volume, uv, length(surface.from_eye) * scale, lighting.reach.x);
    return float4(tone_mapped((radiance * between.a) + between.rgb, lighting.sun_exposure.w), 1.0);
}
