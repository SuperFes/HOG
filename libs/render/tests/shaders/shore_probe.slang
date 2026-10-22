import shore;

[[vk::binding(0, 0)]] Sampler2DArray depths;
// The anchor, east and north as shore_waters takes them, and which way the wind blows; then points
// of sea level from the eye, each with the planet's radius. Under each, the depth of the sea and of
// still water, the fetch there, and the level still water is kept to.
[[vk::binding(1, 0)]] RWStructuredBuffer<float4> asked;
[[vk::binding(2, 0)]] RWStructuredBuffer<float4> found;
[[vk::binding(3, 0)]] Sampler2DArray fetches;

[shader("compute")]
[numthreads(64, 1, 1)]
void compute_main(uint3 id: SV_DispatchThreadID) {
    uint count;
    uint stride;
    found.GetDimensions(count, stride);
    if (id.x >= count) {
        return;
    }
    const float4 point = asked[4 + id.x];
    const float3 waters = shore_waters(depths, asked[0], asked[1], asked[2], point.w, point.xyz);
    const float fetch =
        shore_fetch(fetches, asked[0], asked[1], asked[2], point.w, point.xyz, asked[3].xyz);
    found[id.x] = float4(waters.xy, fetch, waters.z);
}
