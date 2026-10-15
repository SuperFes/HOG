import atmosphere;
import tone;

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

struct Vertex {
    [[vk::location(0)]] float out; // from 0 under the eye to 1 at the rim
    [[vk::location(1)]] float2 towards; // east and north
};

struct Surface {
    float4 position : SV_Position;
    float3 from_eye;
    float3 normal;
};

// Reflected straight back, as water does.
static const float facing_reflection = 0.02;
static const float glint_sharpness = 2000.0;
static const float glint_brightness = 50.0;
// Through the air, the glint is the sun's light reflected by a normalised Blinn-Phong lobe.
static const float glint_lobe = (glint_sharpness + 8.0) / (8.0 * pi);

// As hog::terrain::sea_along and sea_from_eye place it.
[shader("vertex")]
Surface vertex_main(Vertex vertex) {
    const float3 up = push.up_height.xyz;
    const float3 east = push.east_faked.xyz;
    const float faked = push.east_faked.w;
    const float nearest = push.sun_nearest.w;
    const float along = nearest * (pow((push.sky_furthest.w / nearest) + 1.0, vertex.out) - 1.0);

    const float3 towards = (east * vertex.towards.x) + (cross(up, east) * vertex.towards.y);
    const float turned = along / faked;
    const float half = sin(turned / 2.0);
    Surface surface;
    surface.from_eye =
        (up * ((-2.0 * faked * half * half) - push.up_height.w)) + (towards * (faked * sin(turned)));
    surface.position = mul(push.clip_from_eye, float4(surface.from_eye, 1.0));
    surface.normal = (up * cos(turned)) + (towards * sin(turned));
    return surface;
}

// Schlick's: how much is reflected, more the more glancing the view. From under the sea, its
// surface reflects as much back down.
float reflected_at(float3 looking, float3 normal) {
    const float facing = abs(dot(looking, normal));
    return facing_reflection + ((1.0 - facing_reflection) * pow(1.0 - facing, 5.0));
}

// How much of the sun's light the glint reflects towards the eye, but for the reflection.
float glint_at(float3 looking, float3 normal, float3 sun) {
    const float3 halfway = normalize(sun - looking);
    const float lit = saturate(dot(normal, sun) * 10.0);
    return lit * pow(saturate(dot(normal, halfway)), glint_sharpness);
}

[shader("fragment")]
float4 fragment_main(Surface surface) : SV_Target {
    const float3 normal = normalize(surface.normal);
    const float3 looking = normalize(surface.from_eye);
    const float reflected = reflected_at(looking, normal);
    const float glint = glint_brightness * reflected * glint_at(looking, normal, push.sun_nearest.xyz);
    // Premultiplied: the sky by how much is reflected, and the glint over all.
    return float4((push.sky_furthest.rgb * reflected) + glint, reflected);
}

// Reflecting the sky as the eye sees it, and the sun's light through the air, seen through the air
// between, and tone mapped.
[shader("fragment")]
float4 fragment_air(Surface surface) : SV_Target {
    const float3 normal = normalize(surface.normal);
    const float3 looking = normalize(surface.from_eye);
    const float reflected = reflected_at(looking, normal);
    const float3 sun = push.sun_nearest.xyz;
    const float3 up = push.up_height.xyz;
    const float height = lighting.up_height.w;
    const float scale = lighting.sky_scale.w;

    // The reflected view, read from the eye's sky-view table as if seen from the eye.
    const float3 mirrored = reflect(looking, normal);
    const float mu = dot(mirrored, up);
    const float sun_mu = dot(sun, up);
    const float3 flat_view = mirrored - (up * mu);
    const float3 flat_sun = sun - (up * sun_mu);
    const float across = length(flat_view) * length(flat_sun);
    const float cos_azimuth = across > 1e-6 ? dot(flat_view, flat_sun) / across : 1.0;
    const float3 sky = sky_view_table.SampleLevel(sky_view_uv(air, height, mu, cos_azimuth), 0.0).rgb;
    const float3 sunlight = transmittance(air, transmittance_table, 0.0, dot(normal, sun));
    const float3 glint = sunlight * glint_lobe * reflected * glint_at(looking, normal, sun);

    const float4 clip = mul(push.clip_from_eye, float4(surface.from_eye, 1.0));
    const float2 uv = ((clip.xy / clip.w) * 0.5) + 0.5;
    const float4 between =
        aerial_perspective(aerial_volume, uv, length(surface.from_eye) * scale, lighting.reach.x);
    // Premultiplied: over what's beneath, which has the air between already, the reflection seen
    // through the air, and the glint.
    const float3 radiance = (reflected * ((sky * between.a) + between.rgb)) + (glint * between.a);
    return float4(tone_mapped(radiance, lighting.sun_exposure.w), reflected);
}
