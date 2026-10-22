import waves;

[[vk::binding(0, 0)]] ConstantBuffer<Waves> drawn;
// Two to a point: of sea level from the eye, with the footprint it's seen over; and the floor under
// it, with w the fetch over still water there, none over the sea. Where the waves move each, how its surface faces, and how much of it is white with foam
// breaking and left, and the foam's pattern there; and the root mean square of how high the drawn
// waves rise there, the share of the sea white in the surf, and how high the water stands over it.
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
    const float fetch = asked[(2 * id.x) + 1].w;
    const WaveSurface water = wave_surface(drawn, point.xyz, point.w, floor, fetch);
    const Surf surf = surf_at(drawn, water, floor);
    const FoamPattern pattern = foam_pattern_at(
        drawn, water, surf, coarse_foam_at(drawn, point.xyz, point.w), point.xyz, point.w);
    const Foam foam = foam_at(drawn, water, surf, pattern, point.w);
    found[4 * id.x] = float4(water.moved, water.roughness);
    found[(4 * id.x) + 1] = float4(water.normal, foam.breaking);
    found[(4 * id.x) + 2] = float4(foam.left, pattern.coarse, pattern.fine, pattern.fine_seen);
    found[(4 * id.x) + 3] =
        float4(sqrt(water.heave), surf.white,
               water_height_over(drawn, point.xyz, point.w, float4(floor, 0.0), fetch), 0.0);
}
