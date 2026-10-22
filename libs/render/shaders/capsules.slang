import atmosphere;
import shadow;
import tone;
import water;
import waves;

struct Push {
    float4x4 clip_from_eye;
    // xyz the feet from the eye, in metres; w the radius.
    float4 feet_radius;
    // xyz from the feet to the top; w how bright the side away from the sun stays.
    float4 tall_night;
    // rgb its colour; w the eye's height above sea level.
    float4 colour;
    // xyz towards the sun; w the feet's height above sea level.
    float4 sun;
};

[[vk::push_constant]] ConstantBuffer<Push> push;

// When lit through the air.
[[vk::binding(0, 0)]] ConstantBuffer<Air> air;
[[vk::binding(1, 0)]] ConstantBuffer<Lighting> lighting;
[[vk::binding(2, 0)]] Sampler2D transmittance_table;
[[vk::binding(3, 0)]] Sampler2DArray aerial_volume;
// As cloud_march.slang's.
[[vk::binding(4, 0)]] Sampler2D cloud_shadow;
// The sea, as ocean.slang reads it, for how high the water stands at the feet.
[[vk::binding(5, 0)]] ConstantBuffer<Waves> sea;
[[vk::binding(6, 0)]] Sampler2DArray shore_depths;
[[vk::binding(7, 0)]] Sampler2DArray shore_fetches;

// Around, and up each half from its pole to its equator; as hog::render::Capsules counts.
static const uint segments = 16;
static const uint rings = 6;

struct Surface {
    float4 position : SV_Position;
    float3 from_eye;
    float3 normal;
    // Metres over sea level the water stands at the feet, or far below them where it can't reach.
    nointerpolation float water;
};

// The water's surface at the feet is found as the sea draws it seen over so many metres.
static const float wading_footprint = 0.25;
// The waves rise no higher over the water's level than so many times the root mean square of it.
static const float highest_rise = 4.0;
// Far below anything drawn.
static const float no_water = -1.0e6;

// Quads round each band between rows of corners, the lower half's rows then the upper's, the
// band between the halves being the cylinder.
Surface placed(uint index) {
    static const uint2 corners[6] = {uint2(0, 0), uint2(1, 0), uint2(1, 1),
                                     uint2(0, 0), uint2(1, 1), uint2(0, 1)};
    const uint quad = index / 6;
    const uint2 corner = uint2(quad % segments, quad / segments) + corners[index % 6];
    const bool upper = corner.y > rings;
    const float row = float(upper ? corner.y - 1 : corner.y);
    const float latitude = ((row / float(rings)) - 1.0) * (pi / 2.0);
    const float around = float(corner.x) * 2.0 * pi / float(segments);

    const float3 tall = push.tall_night.xyz;
    const float3 up = normalize(tall);
    const float3 other = abs(up.x) < 0.9 ? float3(1.0, 0.0, 0.0) : float3(0.0, 1.0, 0.0);
    const float3 across = normalize(cross(up, other));
    const float3 beside = cross(up, across);
    const float radius = push.feet_radius.w;
    const float3 centre = push.feet_radius.xyz + (up * radius) +
                          (upper ? tall - (up * 2.0 * radius) : float3(0.0));

    Surface surface;
    surface.normal = (((across * cos(around)) + (beside * sin(around))) * cos(latitude)) +
                     (up * sin(latitude));
    surface.from_eye = centre + (surface.normal * radius);
    surface.position = mul(push.clip_from_eye, float4(surface.from_eye, 1.0));
    surface.water = no_water;
    return surface;
}

[shader("vertex")]
Surface vertex_main(uint index : SV_VertexID) {
    return placed(index);
}

// How high the water stands at the feet, as the sea draws it: looked for closely only where its
// waves might reach them.
float water_at_feet() {
    const float3 up = normalize(push.tall_night.xyz);
    const float feet = push.sun.w;
    const float3 over = push.feet_radius.xyz - (up * feet);
    float fetch;
    const float4 floor = floor_at(sea, shore_depths, shore_fetches, over, fetch);
    const WaveSurface water = wave_surface(sea, over, wading_footprint, floor.xyz, fetch);
    if (feet > floor.w + (highest_rise * sqrt(water.heave))) {
        return no_water;
    }
    return water_height_over(sea, over, wading_footprint, floor, fetch);
}

// As vertex_main, with the water at the feet.
[shader("vertex")]
Surface vertex_air(uint index : SV_VertexID) {
    Surface surface = placed(index);
    surface.water = water_at_feet();
    return surface;
}

[shader("fragment")]
float4 fragment_main(Surface surface) : SV_Target {
    const float night = push.tall_night.w;
    const float facing = saturate(dot(normalize(surface.normal), push.sun.xyz));
    return float4(push.colour.rgb * (night + ((1.0 - night) * facing)), 1.0);
}

// Lit by sunlight through the air at the eye's height and the sky's light at the eye, as the
// ground is, seen through the air between, and tone mapped.
[shader("fragment")]
float4 fragment_air(Surface surface) : SV_Target {
    const float3 normal = normalize(surface.normal);
    const float3 up = normalize(push.tall_night.xyz);
    const float3 sun = push.sun.xyz;
    const float scale = lighting.sky_scale.w;
    const float3 clear = transmittance(air, transmittance_table, lighting.up_height.w, dot(up, sun));
    const Shade shade = shade_at(lighting, cloud_shadow, surface.from_eye);
    const float3 sunlight = through_cloud(shade, clear);
    const float3 sky = sky_under_cloud(lighting, shade, clear * saturate(dot(up, sun)));
    const float3 lit =
        (sunlight * saturate(dot(normal, sun))) + (sky * (0.5 + (0.5 * dot(normal, up))));
    // Short enough that up is the same along it, from its feet.
    const float3 feet = push.feet_radius.xyz;
    const float height = push.sun.w + dot(surface.from_eye - feet, up);
    const float3 radiance =
        through_water(push.colour.rgb * lit / pi, ((sunlight * saturate(dot(up, sun))) + sky) / pi,
                      surface.water - height, push.colour.w - surface.water,
                      length(surface.from_eye), dot(-normalize(surface.from_eye), up),
                      sea_lays_water(lighting));

    const float4 clip = mul(push.clip_from_eye, float4(surface.from_eye, 1.0));
    const float2 uv = ((clip.xy / clip.w) * 0.5) + 0.5;
    const float4 between =
        aerial_perspective(aerial_volume, uv, length(surface.from_eye) * scale, lighting.reach.x);
    return float4(tone_mapped((radiance * between.a) + between.rgb, lighting.sun_exposure.w), 1.0);
}
