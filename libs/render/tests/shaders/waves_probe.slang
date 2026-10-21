import waves;

[[vk::binding(0, 0)]] ConstantBuffer<Waves> drawn;
// Two to a point: of sea level from the eye, with the footprint it's seen over; and the floor under
// it. Where the waves move each, how its surface faces, and how much of it is white with foam
// breaking and left, and the foam's pattern there.
[[vk::binding(1, 0)]] RWStructuredBuffer<float4> asked;
[[vk::binding(2, 0)]] RWStructuredBuffer<float4> found;

[shader("compute")]
[numthreads(64, 1, 1)]
void compute_main(uint3 id: SV_DispatchThreadID) {
    uint count;
    uint stride;
    asked.GetDimensions(count, stride);
    if (2 * id.x >= count) {
        return;
    }
    const float4 point = asked[2 * id.x];
    const float3 floor = asked[(2 * id.x) + 1].xyz;
    const WaveSurface water = wave_surface(drawn, point.xyz, point.w, floor);
    const FoamPattern pattern = foam_pattern_at(
        drawn, water, coarse_foam_at(drawn, point.xyz, point.w), point.xyz, point.w);
    const Foam foam = foam_at(drawn, water, pattern, point.w);
    found[3 * id.x] = float4(water.moved, water.roughness);
    found[(3 * id.x) + 1] = float4(water.normal, foam.breaking);
    found[(3 * id.x) + 2] = float4(foam.left, pattern.coarse, pattern.fine, pattern.fine_seen);
}
