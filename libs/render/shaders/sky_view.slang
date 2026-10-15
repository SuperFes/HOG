import atmosphere;

[[vk::binding(0, 0)]] ConstantBuffer<Air> air;
[[vk::binding(1, 0)]] Sampler2D transmittance_table;
[[vk::binding(2, 0)]] Sampler2D scattering_table;
[[vk::binding(3, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> sky_view;

struct Push {
    // The eye's height above the ground, and the cosine of the sun's angle from straight up, both
    // as on the air's own planet.
    float2 height_sun;
};

[[vk::push_constant]] ConstantBuffer<Push> push;

static const uint sky_view_steps = 32;

[shader("compute")]
[numthreads(8, 8, 1)]
void sky_view_main(uint3 id: SV_DispatchThreadID) {
    if (id.x >= sky_view_width || id.y >= sky_view_height) {
        return;
    }
    const float height = min(push.height_sun.x, air.radii.y - air.radii.x);
    const float sun_mu = push.height_sun.y;
    const float2 uv = (float2(id.xy) + 0.5) / float2(sky_view_width, sky_view_height);
    const float3 view = sky_view_direction(air, height, uv);
    const float3 sun = float3(sqrt(saturate(1.0 - (sun_mu * sun_mu))), 0.0, sun_mu);
    const Scattered scattered = march(air, transmittance_table, scattering_table, height, view, sun,
                                      1e30, sky_view_steps);
    sky_view[id.xy] = float4(scattered.light, 1.0);
}

