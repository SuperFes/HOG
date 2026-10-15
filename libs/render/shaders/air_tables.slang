import atmosphere;

[[vk::binding(0, 0)]] ConstantBuffer<Air> air;
[[vk::binding(1, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> table;
[[vk::binding(2, 0)]] Sampler2D transmittance_table;

static const uint transmittance_steps = 100;
static const uint scattering_directions = 64;
static const uint scattering_steps = 20;

[shader("compute")]
[numthreads(8, 8, 1)]
void transmittance_main(uint3 id: SV_DispatchThreadID) {
    if (id.x >= transmittance_width || id.y >= transmittance_height) {
        return;
    }
    const float2 uv =
        (float2(id.xy) + 0.5) / float2(transmittance_width, transmittance_height);
    float height;
    float mu;
    transmittance_ray(air, uv, height, mu);
    const float step = to_top(air, height, mu) / transmittance_steps;
    float3 depth = float3(0.0);
    for (uint at = 0; at < transmittance_steps; ++at) {
        depth += medium_at(air, height_along(air, height, mu, (at + 0.5) * step)).extinction * step;
    }
    table[id.xy] = float4(exp(-depth), 1.0);
}

// Hillaire's approximation: light scattered twice, from every direction evenly, and the share of
// light scattered again, summed as a geometric series.
[shader("compute")]
[numthreads(8, 8, 1)]
void scattering_main(uint3 id: SV_DispatchThreadID) {
    if (any(id.xy >= scattering_size)) {
        return;
    }
    const float2 uv = (float2(id.xy) + 0.5) / scattering_size;
    const float height = (air.radii.y - air.radii.x) * unit_from_texel(uv.y, scattering_size);
    const float sun_mu = (2.0 * unit_from_texel(uv.x, scattering_size)) - 1.0;
    const float3 sun = float3(sqrt(1.0 - (sun_mu * sun_mu)), 0.0, sun_mu);
    const float radius = air.radii.x + height;
    const float golden = pi * (3.0 - sqrt(5.0));
    const float isotropic = 1.0 / (4.0 * pi);

    float3 twice = float3(0.0);
    float3 again = float3(0.0);
    for (uint direction_at = 0; direction_at < scattering_directions; ++direction_at) {
        const float z = 1.0 - (((2.0 * direction_at) + 1.0) / scattering_directions);
        const float across = sqrt(1.0 - (z * z));
        const float3 direction =
            float3(across * cos(golden * direction_at), across * sin(golden * direction_at), z);
        const bool ground = meets_ground(air, height, z);
        const float length = to_end(air, height, z);
        const float step = length / scattering_steps;
        float3 passed = float3(1.0);
        for (uint at = 0; at < scattering_steps; ++at) {
            const float along = (at + 0.5) * step;
            const float point_height = height_along(air, height, z, along);
            // Up at the point, from the eye's up turned towards the ray.
            const float3 up = normalize(float3(0.0, 0.0, radius) + (direction * along));
            const Medium medium = medium_at(air, point_height);
            const float3 sunlit =
                transmittance(air, transmittance_table, point_height, dot(up, sun));
            const float3 through = exp(-medium.extinction * step);
            twice += passed * within_step(medium.scattering() * sunlit * isotropic,
                                          medium.extinction, through, step);
            again += passed * within_step(medium.scattering(), medium.extinction, through, step);
            passed *= through;
        }
        if (ground) {
            const float lit = dot(normalize(float3(0.0, 0.0, radius) + (direction * length)), sun);
            twice += passed * transmittance(air, transmittance_table, 0.0, lit) * max(lit, 0.0) *
                     air.ground_albedo.xyz / pi;
        }
    }
    twice /= scattering_directions;
    again /= scattering_directions;
    table[id.xy] = float4(twice / (1.0 - again), 1.0);
}
