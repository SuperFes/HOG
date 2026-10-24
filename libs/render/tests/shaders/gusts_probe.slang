import gusts;
import lush;

// Five to a case: a point from the eye with w the wind's speed on the whole; where the gusts'
// field lies from the eye in its own axes with w the seconds plants have swayed for; which way is
// downwind with w how far into its sway grass at the eye is; which way is up; and where fields
// fixed to the ground lie from the eye. Two found to a case: x how hard the gusts blow there, y the
// wind's speed there, z how much further grass is laid over there, w that and its swaying, seen
// whole; then x how far a bush's top is held over, y a tree's, z how thick grass grows there, and
// w how tall it stands for it.
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
    const float4 point = asked[5 * id.x];
    const float4 anchor = asked[(5 * id.x) + 1];
    const float4 along = asked[(5 * id.x) + 2];
    const float4 up = asked[(5 * id.x) + 3];
    const float4 ground = asked[(5 * id.x) + 4];
    const GustFrame frame = { anchor.xyz, along.xyz, up.xyz };
    const float4 fields = gust_fields(point.xyz, frame);
    const float there = fields.x;
    const float gust = gust_of(there);
    const float phase = sway_phase(along.w, point.xyz, frame, fields);
    found[2 * id.x] = float4(gust, gusting(point.w, gust), gust_laid(point.w, gust),
                             gust_sheen(point.w, fields, phase, 1.0));
    const float thick = thickness(point.xyz, ground.xyz);
    found[(2 * id.x) + 1] = float4(bush_sway(anchor.w, point.w, there, 0.0),
                                   tree_sway(anchor.w, point.w, there, 0.0), thick,
                                   thick_height(thick));
}
