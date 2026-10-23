import gusts;

// Two to a case: a point from the eye with w the wind's speed on the whole, and where the gusts'
// field lies from it with w the seconds plants have swayed for. Two found to a case: x how hard
// the gusts blow there, y the wind's speed there, z how much further grass is laid over there, w
// that and its swaying, seen whole; then x how far a bush's top is held over, y a tree's.
[[vk::binding(0, 0)]] RWStructuredBuffer<float4> asked;
[[vk::binding(1, 0)]] RWStructuredBuffer<float4> found;

[shader("compute")]
[numthreads(64, 1, 1)]
void compute_main(uint3 id: SV_DispatchThreadID) {
    uint count;
    uint stride;
    found.GetDimensions(count, stride);
    if (id.x >= count / 2) {
        return;
    }
    const float4 point = asked[2 * id.x];
    const float4 field = asked[(2 * id.x) + 1];
    const float there = gust_field(point.xyz, field.xyz);
    const float gust = gust_of(there);
    found[2 * id.x] = float4(gust, gusting(point.w, gust), gust_laid(point.w, gust),
                             gust_sheen(point.w, there, field.w, 1.0));
    found[(2 * id.x) + 1] = float4(bush_sway(field.w, point.w, there, 0.0),
                                   tree_sway(field.w, point.w, there, 0.0), 0.0, 0.0);
}
