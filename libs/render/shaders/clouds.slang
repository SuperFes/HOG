import atmosphere;
import tone;
import weather;

[[vk::binding(0, 0)]] ConstantBuffer<Lighting> lighting;
[[vk::binding(1, 0)]] Sampler2D<float> depth_image;
// As cloud_resolve.slang and cloud_march.slang leave them.
[[vk::binding(2, 0)]] Sampler2D marched;
[[vk::binding(3, 0)]] Sampler2D<float2> distances;

struct Push {
    float4x4 eye_from_clip;
    // xyz up through the eye, w its height above sea level.
    float4 up_height;
    // y the faked radius drawing bends it onto.
    float4 radii;
};

[[vk::push_constant]] ConstantBuffer<Push> push;

// Nothing drawn is behind the sky, which sky.slang draws at 1e-7.
static const float sky_depth = 2e-7;
// Of how far what's drawn here is, the share a marched texel's must be to be blended in.
static const float fully_marched = 0.9;

struct Layer {
    float4 position : SV_Position;
    float2 clip;
};

// A triangle covering the whole picture.
[shader("vertex")]
Layer vertex_main(uint id: SV_VertexID) {
    const float2 clip = (float2((id << 1) & 2, id & 2) * 2.0) - 1.0;
    Layer layer;
    layer.position = float4(clip, 0.0, 1.0);
    layer.clip = clip;
    return layer;
}

[shader("fragment")]
float4 fragment_main(Layer layer) : SV_Target {
    const float4 far = mul(push.eye_from_clip, float4(layer.clip, 1.0, 1.0));
    const float3 view = normalize(far.xyz / far.w);
    const float3 up = push.up_height.xyz;
    const float eye_height = push.up_height.w;
    const float faked = push.radii.y;
    const float centre_to_eye = faked + eye_height;
    const float mu = dot(view, up);

    // How far the view reaches: to what's drawn, else to the ground.
    float reach = to_layer(mu, centre_to_eye, eye_height, 0.0, faked);
    if (reach <= 0.0) {
        reach = 1e30;
    }
    const float depth = depth_image.Load(int3(int2(layer.position.xy), 0));
    float drawn_reach = 1e30;
    if (depth > sky_depth) {
        const float4 drawn = mul(push.eye_from_clip, float4(layer.clip, depth, 1.0));
        drawn_reach = length(drawn.xyz / drawn.w);
        reach = min(reach, drawn_reach);
    }

    // The clouds marched at a share of the picture's size, blended from the four nearest, but those
    // of them beyond what's drawn here; and leaving out those marched short of it, stopped by
    // something nearer, as they miss what's between.
    uint width;
    uint height;
    marched.GetDimensions(width, height);
    uint full_width;
    uint full_height;
    depth_image.GetDimensions(full_width, full_height);
    const float2 at = (layer.position.xy * float2(width, height) / float2(full_width, full_height)) -
                      0.5;
    const float2 corner = floor(at);
    const float2 within = at - corner;
    float4 seen = float4(0.0);
    float kept = 0.0;
    for (uint tap = 0; tap < 4; ++tap) {
        const int2 offset = int2(int(tap & 1), int(tap >> 1));
        const int2 texel = clamp(int2(corner) + offset, int2(0), int2(width - 1, height - 1));
        const float2 weights = lerp(1.0 - within, within, float2(offset));
        const float2 marched_to = distances.Load(int3(texel, 0));
        if (marched_to.y < drawn_reach * fully_marched) {
            continue;
        }
        kept += weights.x * weights.y;
        if (marched_to.x < reach) {
            seen += marched.Load(int3(texel, 0)) * (weights.x * weights.y);
        }
    }
    seen /= max(kept, 1e-6);
    if (seen.a <= 0.0) {
        discard;
    }
    return float4(tone_mapped(seen.rgb / seen.a, lighting.sun_exposure.w) * seen.a, seen.a);
}
