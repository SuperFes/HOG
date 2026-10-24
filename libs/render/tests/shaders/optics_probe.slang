import water;

// One to a case: what the water carries. Three found to a case: how it dims, hides and colours,
// as optics has it.
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
    const Optics water = optics(asked[id.x / 3]);
    const uint which = id.x % 3;
    found[id.x] = float4(which == 0 ? water.dimming : (which == 1 ? water.hiding : water.colour), 0.0);
}
