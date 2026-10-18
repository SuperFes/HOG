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
// The share by which what's marched round it may be nearer or further and still be blurred into it.
static const float alike = 0.25;
// How many standard deviations of what's marched round it the history may stray.
static const float spreads = 1.25;

// `image` at `uv` through a Catmull-Rom spline, from five of its bilinear samples rather than
// sixteen texels, so the history's not blurred further each time it's moved by part of a texel.
float4 sharply(Sampler2D image, float2 uv, float2 size) {
    const float2 position = uv * size;
    const float2 centre = floor(position - 0.5) + 0.5;
    const float2 f = position - centre;
    const float2 w0 = f * (-0.5 + (f * (1.0 - (0.5 * f))));
    const float2 w1 = 1.0 + (f * f * (-2.5 + (1.5 * f)));
    const float2 w2 = f * (0.5 + (f * (2.0 - (1.5 * f))));
    const float2 w3 = f * f * (-0.5 + (0.5 * f));
    const float2 w12 = w1 + w2;
    const float2 at0 = (centre - 1.0) / size;
    const float2 at12 = (centre + (w2 / w12)) / size;
    const float2 at3 = (centre + 2.0) / size;
    const float4 sum = (image.SampleLevel(float2(at12.x, at0.y), 0.0) * (w12.x * w0.y)) +
                       (image.SampleLevel(float2(at0.x, at12.y), 0.0) * (w0.x * w12.y)) +
                       (image.SampleLevel(at12, 0.0) * (w12.x * w12.y)) +
                       (image.SampleLevel(float2(at3.x, at12.y), 0.0) * (w3.x * w12.y)) +
                       (image.SampleLevel(float2(at12.x, at3.y), 0.0) * (w12.x * w3.y));
    const float weight = (w12.x * w0.y) + (w0.x * w12.y) + (w12.x * w12.y) + (w3.x * w12.y) +
                         (w12.x * w3.y);
    return sum / weight;
}

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

    // Of what's marched round it now: the spread, and that blurred, where there's nothing before to
    // blend it with and its noise would show; but only from what's about as far, or nothing, as
    // what's further may be hidden here.
    const float distance = distances[id.xy];
    float4 sum = float4(0.0);
    float4 squares = float4(0.0);
    float4 smoothed = float4(0.0);
    float weights = 0.0;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            const int2 next = clamp(int2(id.xy) + int2(dx, dy), int2(0), int2(width - 1, height - 1));
            const float4 there = marched[next];
            sum += there;
            squares += there * there;
            const float there_distance = distances[next];
            if (there_distance <= 0.0 ||
                abs(there_distance - distance) <= alike * max(there_distance, distance)) {
                const float weight = (dx == 0 ? 2.0 : 1.0) * (dy == 0 ? 2.0 : 1.0);
                smoothed += there * weight;
                weights += weight;
            }
        }
    }
    smoothed /= weights;

    // Where it was in the last frame: as far along its view as the cloud is, or as far as the sky.
    const float2 uv = (float2(id.xy) + 0.5) / float2(width, height);
    const float4 far = mul(push.eye_from_clip, float4((uv * 2.0) - 1.0, 1.0, 1.0));
    const float3 view = normalize(far.xyz / far.w);
    const float4 seen = distance > 0.0 ? float4(view * distance, 1.0) : float4(view, 0.0);
    const float4 clip = mul(push.previous_clip_from_eye, seen);
    const float2 then = ((clip.xy / max(clip.w, 1e-30)) * 0.5) + 0.5;
    if (clip.w <= 0.0 || any(then < 0.0) || any(then > 1.0)) {
        blended[id.xy] = now.a > 0.0 ? smoothed : now;
        return;
    }

    // Kept within the spread of what's marched round it now, so what's moved or gone doesn't
    // linger: closer than its least and most, which the march's noise spreads wide.
    const float4 mean = sum / 9.0;
    const float4 spread = sqrt(max((squares / 9.0) - (mean * mean), 0.0)) * spreads;
    const float4 least = mean - spread;
    const float4 most = mean + spread;
    const float4 before = clamp(sharply(history, then, float2(width, height)), least, most);
    blended[id.xy] = lerp(before, now, fresh);
}
