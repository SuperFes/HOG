import gusts;

// Two to a case: a point from the eye, and where the gusts' field lies from it. One found to a
// case: x how hard the gusts blow there.
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
    found[id.x] = float4(gust_at(asked[2 * id.x].xyz, asked[(2 * id.x) + 1].xyz), 0.0, 0.0, 0.0);
}
