[[vk::binding(0, 0)]] Sampler3D volume;
// xyz where in the volume, w the mip.
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
    const float4 at = asked[id.x];
    found[id.x] = volume.SampleLevel(at.xyz, at.w);
}
