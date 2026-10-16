import projection;

// The planet's up, then the ground's normal, two to a case.
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
    found[id.x] = float4(plane_shares(asked[2 * id.x].xyz, asked[(2 * id.x) + 1].xyz), 0.0);
}
