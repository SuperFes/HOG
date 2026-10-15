import atmosphere;

[[vk::binding(0, 0)]] ConstantBuffer<Air> air;
[[vk::binding(1, 0)]] Sampler2D transmittance_table;
[[vk::binding(2, 0)]] Sampler2D scattering_table;
[[vk::binding(3, 0)]] ConstantBuffer<Lighting> lighting;
[[vk::binding(4, 0)]] [[vk::image_format("rgba16f")]] RWTexture2DArray<float4> volume;

static const uint aerial_steps = 12;

// Each texel holds the light the air adds along its pixel's view out to its slice's end, and the
// share of light it lets through, averaged over red, green and blue.
[shader("compute")]
[numthreads(8, 8, 1)]
void aerial_main(uint3 id: SV_DispatchThreadID) {
    if (any(id >= uint3(aerial_size, aerial_size, aerial_slices))) {
        return;
    }
    const float2 clip = (((float2(id.xy) + 0.5) / aerial_size) * 2.0) - 1.0;
    const float4 far = mul(lighting.eye_from_clip, float4(clip, 1.0, 1.0));
    const float3x3 local = frame_of(lighting.up_height.xyz);
    const float3 view = mul(local, normalize(far.xyz / far.w));
    const float3 sun = mul(local, lighting.sun_exposure.xyz);
    const Scattered scattered =
        march(air, transmittance_table, scattering_table, lighting.up_height.w, view, sun,
              aerial_slice_end(id.z, lighting.reach.x), aerial_steps);
    const float3 passed = scattered.transmittance;
    volume[id] = float4(scattered.light, (passed.r + passed.g + passed.b) / 3.0);
}
