import water;

// Three to a case: the way the eye looks; the surface's normal; its level up, and how far beyond
// the surface the point lies. One found to a case: how far bent_by_waves moves it.
[[vk::binding(0, 0)]] RWStructuredBuffer<float4> asked;
[[vk::binding(1, 0)]] RWStructuredBuffer<float4> found;

[shader("compute")]
[numthreads(64, 1, 1)]
void compute_main(uint3 id: SV_DispatchThreadID) {
    uint count;
    uint stride;
    found.GetDimensions(count, stride);
    if (id.x >= count) {
        return;
    }
    const float4 looking = asked[3 * id.x];
    const float4 normal = asked[(3 * id.x) + 1];
    const float4 level = asked[(3 * id.x) + 2];
    found[id.x] = float4(bent_by_waves(looking.xyz, normal.xyz, level.xyz, level.w), 0.0);
}
