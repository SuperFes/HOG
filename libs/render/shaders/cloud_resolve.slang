// What was marched this frame, blended with what was for the frames before, as seen from where the
// eye was.

// As cloud_march.slang leaves them.
[[vk::binding(0, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> marched;
[[vk::binding(1, 0)]] [[vk::image_format("r32f")]] RWTexture2D<float> distances;
// The last frame's blend.
[[vk::binding(2, 0)]] Sampler2D history;
[[vk::binding(3, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> blended;

struct Push {
    float4x4 eye_from_clip;
    // From this frame's eye to the last frame's clip space; all 0 when there's nothing to blend.
    float4x4 previous_clip_from_eye;
};

[[vk::push_constant]] ConstantBuffer<Push> push;

// Of each frame, the share it adds to the blend.
static const float fresh = 0.15;
// How many standard deviations of what's marched round it the history may stray.
static const float spreads = 1.25;

[shader("compute")]
[numthreads(8, 8, 1)]
void resolve_main(uint3 id: SV_DispatchThreadID) {
    uint width;
    uint height;
    marched.GetDimensions(width, height);
    if (id.x >= width || id.y >= height) {
        return;
    }
    const float4 now = marched[id.xy];

    // Where it was in the last frame: as far along its view as the cloud is, or as far as the sky.
    const float2 uv = (float2(id.xy) + 0.5) / float2(width, height);
    const float4 far = mul(push.eye_from_clip, float4((uv * 2.0) - 1.0, 1.0, 1.0));
    const float3 view = normalize(far.xyz / far.w);
    const float distance = distances[id.xy];
    const float4 seen = distance > 0.0 ? float4(view * distance, 1.0) : float4(view, 0.0);
    const float4 clip = mul(push.previous_clip_from_eye, seen);
    if (clip.w <= 0.0) {
        blended[id.xy] = now;
        return;
    }
    const float2 then = ((clip.xy / clip.w) * 0.5) + 0.5;
    if (any(then < 0.0) || any(then > 1.0)) {
        blended[id.xy] = now;
        return;
    }

    // Kept within the spread of what's marched round it now, so what's moved or gone doesn't
    // linger: closer than its least and most, which the march's noise spreads wide.
    float4 sum = float4(0.0);
    float4 squares = float4(0.0);
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            const int2 next = clamp(int2(id.xy) + int2(dx, dy), int2(0), int2(width - 1, height - 1));
            const float4 there = marched[next];
            sum += there;
            squares += there * there;
        }
    }
    const float4 mean = sum / 9.0;
    const float4 spread = sqrt(max((squares / 9.0) - (mean * mean), 0.0)) * spreads;
    const float4 least = mean - spread;
    const float4 most = mean + spread;
    const float4 before = clamp(history.SampleLevel(then, 0.0), least, most);
    blended[id.xy] = lerp(before, now, fresh);
}
