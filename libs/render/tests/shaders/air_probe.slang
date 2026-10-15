import atmosphere;

[[vk::binding(0, 0)]] ConstantBuffer<Air> air;
[[vk::binding(1, 0)]] Sampler2D transmittance_table;
[[vk::binding(2, 0)]] Sampler2D scattering_table;
// xy a height and direction to read the transmittance table at, zw the multiple-scattering table.
[[vk::binding(3, 0)]] RWStructuredBuffer<float4> asked;
// Transmittance and multiple scattering, a pair each.
[[vk::binding(4, 0)]] RWStructuredBuffer<float4> found;

[shader("compute")]
[numthreads(64, 1, 1)]
void compute_main(uint3 id: SV_DispatchThreadID) {
    uint count;
    uint stride;
    asked.GetDimensions(count, stride);
    if (id.x >= count) {
        return;
    }
    const float4 at = asked[id.x];
    found[2 * id.x] = float4(transmittance(air, transmittance_table, at.x, at.y), 0.0);
    found[(2 * id.x) + 1] = float4(multiple_scattering(air, scattering_table, at.z, at.w), 0.0);
}
