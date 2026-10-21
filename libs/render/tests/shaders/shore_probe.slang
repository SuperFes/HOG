import shore;

[[vk::binding(0, 0)]] Sampler2DArray depths;
// The anchor, east and north as shore_depth takes them, then points of sea level from the eye,
// each with the planet's radius; the depth under each.
[[vk::binding(1, 0)]] RWStructuredBuffer<float4> asked;
[[vk::binding(2, 0)]] RWStructuredBuffer<float4> found;

[shader("compute")]
[numthreads(64, 1, 1)]
void compute_main(uint3 id: SV_DispatchThreadID) {
    uint count;
    uint stride;
    found.GetDimensions(count, stride);
    if (id.x >= count) {
        return;
    }
    const float4 point = asked[3 + id.x];
    found[id.x] = float4(shore_depth(depths, asked[0], asked[1], asked[2], point.w, point.xyz), 0.0,
                         0.0, 0.0);
}
