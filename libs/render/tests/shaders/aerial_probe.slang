import atmosphere;

[[vk::binding(0, 0)]] Sampler2DArray volume;
// xy where on the picture, z the air's metres away, w the volume's reach.
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
    found[id.x] = aerial_perspective(volume, at.xy, at.z, at.w);
}
