import waves;

[[vk::binding(0, 0)]] ConstantBuffer<Waves> drawn;
[[vk::binding(1, 0)]] Sampler2D kept;
// Points of sea level from the eye over deep water, each with the footprint it's seen over. How
// much of each is white with foam left, with what's kept round the eye and as likely without it;
// and what's kept there.
[[vk::binding(2, 0)]] RWStructuredBuffer<float4> asked;
[[vk::binding(3, 0)]] RWStructuredBuffer<float4> found;

[shader("compute")]
[numthreads(64, 1, 1)]
void compute_main(uint3 id: SV_DispatchThreadID) {
    uint count;
    uint stride;
    asked.GetDimensions(count, stride);
    if (id.x >= count) {
        return;
    }
    const float4 point = asked[id.x];
    const float3 deep = float3(1.0e6, 0.0, 0.0);
    const WaveSurface water = wave_surface(drawn, point.xyz, point.w, deep);
    const Surf surf = surf_at(drawn, water, deep);
    const float2 here = kept_foam_at(drawn, kept, point.xyz);
    const FoamPattern pattern =
        foam_pattern_at(drawn, water, surf, coarse_foam_at(drawn, point.xyz, point.w), point.xyz,
                        point.w, here.x > 0.0);
    const Foam foam = foam_at(drawn, water, surf, pattern, point.w);
    found[id.x] = float4(left_with_kept(foam, pattern, here, point.w), foam.left, here);
}
