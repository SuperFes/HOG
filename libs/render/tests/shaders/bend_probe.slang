import bend;

// Three to a case: a point from the eye, and an angle; the unit up through the eye, and its height
// above sea level; the planet's radius, the one it's bent to, and a y and x. Two found to a case:
// the point bent, then the angle's sine and the arctangent of y over x.
[[vk::binding(0, 0)]] RWStructuredBuffer<float4> asked;
[[vk::binding(1, 0)]] RWStructuredBuffer<float4> found;

[shader("compute")]
[numthreads(64, 1, 1)]
void compute_main(uint3 id: SV_DispatchThreadID) {
    uint count;
    uint stride;
    found.GetDimensions(count, stride);
    if (2 * id.x >= count) {
        return;
    }
    const float4 point = asked[3 * id.x];
    const float4 up = asked[(3 * id.x) + 1];
    const float4 more = asked[(3 * id.x) + 2];
    found[2 * id.x] = float4(bent(point.xyz, up, more.xy), 0.0);
    found[(2 * id.x) + 1] = float4(small_sin(point.w), small_atan(more.z, more.w), 0.0, 0.0);
}
