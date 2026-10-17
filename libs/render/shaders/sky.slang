import atmosphere;
import tone;

[[vk::binding(0, 0)]] ConstantBuffer<Air> air;
[[vk::binding(1, 0)]] Sampler2D transmittance_table;
[[vk::binding(2, 0)]] Sampler2D scattering_table;
[[vk::binding(3, 0)]] Sampler2D sky_view_table;
[[vk::binding(4, 0)]] Sampler2DArray aerial_volume;

struct Push {
    float4x4 eye_from_clip;
    // xyz up through the eye, w its height above the ground as on the air's own planet.
    float4 up_height;
    // xyz towards the sun, w how much brighter the picture is than the light reaching it.
    float4 sun_exposure;
};

[[vk::push_constant]] ConstantBuffer<Push> push;

// The sun's, as seen from Earth, and the radiance across it for sunlight of irradiance 1.
static const float sun_angular_radius = 0.004675;
static const float sun_radiance = 1.0 / (pi * sun_angular_radius * sun_angular_radius);
// Just in front of the cleared depth, so the sky is drawn there and anything drawn after hides it.
static const float sky_depth = 1e-7;
static const uint march_steps = 48;

struct Sky {
    float4 position : SV_Position;
    float2 clip;
};

// A triangle covering the whole picture.
[shader("vertex")]
Sky vertex_main(uint id: SV_VertexID) {
    const float2 clip = (float2((id << 1) & 2, id & 2) * 2.0) - 1.0;
    Sky sky;
    sky.position = float4(clip, sky_depth, 1.0);
    sky.clip = clip;
    return sky;
}

[shader("fragment")]
float4 fragment_main(Sky sky) : SV_Target {
    const float4 far = mul(push.eye_from_clip, float4(sky.clip, 1.0, 1.0));
    const float3 view = normalize(far.xyz / far.w);
    const float3 up = push.up_height.xyz;
    const float height = push.up_height.w;
    const float3 sun = push.sun_exposure.xyz;
    const float mu = dot(view, up);
    const float sun_mu = dot(sun, up);

    float3 light;
    if (height > air.radii.y - air.radii.x) {
        // Above the air the table can't be made, so the sky is marched for each pixel.
        const float3 east = normalize(abs(up.z) < 0.9 ? cross(float3(0.0, 0.0, 1.0), up)
                                                      : cross(float3(1.0, 0.0, 0.0), up));
        const float3 north = cross(up, east);
        const float3x3 local = float3x3(east, north, up);
        light = march(air, transmittance_table, scattering_table, height, mul(local, view),
                      mul(local, sun), 1e30, march_steps)
                    .light;
    } else {
        const float3 flat_view = view - (up * mu);
        const float3 flat_sun = sun - (up * sun_mu);
        const float across = length(flat_view) * length(flat_sun);
        const float cos_azimuth = across > 1e-6 ? dot(flat_view, flat_sun) / across : 1.0;
        light = sky_view_table.SampleLevel(sky_view_uv(air, height, mu, cos_azimuth), 0.0).rgb;
    }
    if (dot(view, sun) > cos(sun_angular_radius)) {
        light += sun_radiance * transmittance(air, transmittance_table, height, mu);
    }
    const float4 haze =
        aerial_volume.SampleLevel(float3((sky.clip * 0.5) + 0.5, float(aerial_haze)), 0.0);
    light = (light * haze.a) + haze.rgb;
    return float4(tone_mapped(light, push.sun_exposure.w), 1.0);
}
