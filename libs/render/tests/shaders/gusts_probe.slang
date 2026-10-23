import gusts;

// Two to a case: a point from the eye with w the wind's speed on the whole, and where the gusts'
// field lies from it. One found to a case: x how hard the gusts blow there, y the wind's speed
// there.
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
    const float4 point = asked[2 * id.x];
    const float gust = gust_at(point.xyz, asked[(2 * id.x) + 1].xyz);
    found[id.x] = float4(gust, gusting(point.w, gust), 0.0, 0.0);
}
