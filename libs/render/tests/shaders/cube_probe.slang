[[vk::binding(0, 0)]] SamplerCube cube;
// xyz the way to look, in the cube's axes.
[[vk::binding(1, 0)]] RWStructuredBuffer<float4> asked;
[[vk::binding(2, 0)]] RWStructuredBuffer<float4> found;

[shader("compute")]
[numthreads(64, 1, 1)]
void compute_main(uint3 id: SV_DispatchThreadID) {
    uint count;
    uint stride;
    asked.GetDimensions(count, stride);
    if (id.x >= count) {
        return;
    }
    found[id.x] = cube.SampleLevel(asked[id.x].xyz, 0.0);
}
