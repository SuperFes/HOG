struct Push {
    uint2 size;
};

[[vk::push_constant]] ConstantBuffer<Push> push;

[[vk::binding(0, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> written;
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> indices;

// Each texel's place across the image, and its index into the buffer.
[shader("compute")]
[numthreads(8, 8, 1)]
void compute_main(uint3 id: SV_DispatchThreadID) {
    if (any(id.xy >= push.size)) {
        return;
    }
    written[id.xy] = float4(float2(id.xy) / float2(push.size), 0.5, 1.0);
    indices[(id.y * push.size.x) + id.x] = (id.y * push.size.x) + id.x;
}

[[vk::binding(0, 0)]] Sampler2D read;
[[vk::binding(1, 0)]] RWStructuredBuffer<float4> samples;

// The image sampled at each texel's centre.
[shader("compute")]
[numthreads(8, 8, 1)]
void sample_main(uint3 id: SV_DispatchThreadID) {
    if (any(id.xy >= push.size)) {
        return;
    }
    samples[(id.y * push.size.x) + id.x] =
        read.SampleLevel((float2(id.xy) + 0.5) / float2(push.size), 0.0);
}
