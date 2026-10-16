import atmosphere;
import tone;

struct Push {
    float4x4 clip_from_eye;
    // xyz the feet from the eye, in metres; w the radius.
    float4 feet_radius;
    // xyz from the feet to the top; w how bright the side away from the sun stays.
    float4 tall_night;
    float4 colour;
    // xyz towards the sun.
    float4 sun;
};

[[vk::push_constant]] ConstantBuffer<Push> push;

// When lit through the air.
[[vk::binding(0, 0)]] ConstantBuffer<Air> air;
[[vk::binding(1, 0)]] ConstantBuffer<Lighting> lighting;
[[vk::binding(2, 0)]] Sampler2D transmittance_table;
[[vk::binding(3, 0)]] Sampler2DArray aerial_volume;

// Around, and up each half from its pole to its equator; as hog::render::Capsules counts.
static const uint segments = 16;
static const uint rings = 6;

struct Surface {
    float4 position : SV_Position;
    float3 from_eye;
    float3 normal;
};

// Quads round each band between rows of corners, the lower half's rows then the upper's, the
// band between the halves being the cylinder.
[shader("vertex")]
Surface vertex_main(uint index : SV_VertexID) {
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
    const float3 sunlight =
        transmittance(air, transmittance_table, lighting.up_height.w, dot(up, sun));
    const float3 lit = (sunlight * saturate(dot(normal, sun))) +
                       (lighting.sky_scale.rgb * (0.5 + (0.5 * dot(normal, up))));
    const float3 radiance = push.colour.rgb * lit / pi;

    const float4 clip = mul(push.clip_from_eye, float4(surface.from_eye, 1.0));
    const float2 uv = ((clip.xy / clip.w) * 0.5) + 0.5;
    const float4 between =
        aerial_perspective(aerial_volume, uv, length(surface.from_eye) * scale, lighting.reach.x);
    return float4(tone_mapped((radiance * between.a) + between.rgb, lighting.sun_exposure.w), 1.0);
}
