import atmosphere;
import shadow;
import tone;

// Trees baked by the tree lab (docs/flora-assets.md), each drawn whole and rigid where its trunk
// meets the ground: its meshes and leaves read straight from the file's bytes, every level of
// detail a prefix of them, the pieces kept grown to hold the crown's area.

// As hog::render::Trees' Push.
struct Push {
    // xyz the trunk's foot from the eye, bent; w metres a pixel spans there.
    float4 foot_pixel;
    // The tree's own axes, from the lab's (x, y up, z) to the planet's, scaled by its size; w: its
    // tint, -1 to 1, the part's translucency, its cutoff.
    float4 x_tint;
    float4 y_translucency;
    float4 z_cutoff;
    // The bounds positions are quantized over; w: the part's pixels a piece is kept at, and how
    // a piece's area grows with its size (0 never thins).
    float4 lo_target;
    float4 hi_dims;
    // xy the uvs' scale; z 1 where pieces are calmed toward their neighbours' means; w 1 where
    // the albedo's alpha is bark's height.
    float4 uv_calm;
};

[[vk::push_constant]] ConstantBuffer<Push> push;

// As hog::render::Trees' Frame.
struct Frame {
    float4x4 clip_from_eye;
    float4 sun; // xyz towards it
};

// As hog::render::Trees' Template: a species' leaf, along its across, way and face axes per
// metre of its length; w unused.
struct Template {
    float4 corners[6];
    float4 uvs[3];
    uint4 indices[3];
    float4 colour_range;
};

[[vk::binding(0, 0)]] ConstantBuffer<Air> air;
[[vk::binding(1, 0)]] ConstantBuffer<Lighting> lighting;
[[vk::binding(2, 0)]] Sampler2D transmittance_table;
[[vk::binding(3, 0)]] Sampler2DArray aerial_volume;
[[vk::binding(4, 0)]] Sampler2D cloud_shadow;
[[vk::binding(5, 0)]] ConstantBuffer<Frame> frame;
// A mesh's vertices or a part's leaves, and a mesh's pieces, as the file lays them.
[[vk::binding(6, 0)]] StructuredBuffer<uint> records;
[[vk::binding(7, 0)]] StructuredBuffer<uint> pieces;
[[vk::binding(8, 0)]] Sampler2D albedo;
[[vk::binding(9, 0)]] ConstantBuffer<Template> leaf_shape;

struct Surface {
    float4 position : SV_Position;
    float3 from_eye;
    float3 normal;
    float3 colour;
    float2 uv;
    // Of the sun, how much the tree's own leaves let through to here.
    float lit;
};

float2 unorm16x2(uint word) {
    return float2(word & 0xffffu, word >> 16u) / 65535.0;
}

float4 unorm8x4(uint word) {
    return float4(word & 0xffu, (word >> 8u) & 0xffu, (word >> 16u) & 0xffu, word >> 24u) / 255.0;
}

float snorm8(uint byte) {
    return max(float(int(byte << 24u) >> 24) / 127.0, -1.0);
}

float2 snorm16x2(uint word) {
    return max(float2(int(word << 16u) >> 16, int(word) >> 16) / 32767.0, -1.0);
}

// A half float's low 16 bits, without needing the device to take halves.
float from_half(uint bits) {
    const uint exponent = (bits >> 10u) & 0x1fu;
    const uint mantissa = bits & 0x3ffu;
    const float magnitude = exponent == 0u
                                ? float(mantissa) * 0x1p-24
                                : asfloat(((exponent + 112u) << 23u) | (mantissa << 13u));
    return (bits & 0x8000u) != 0u ? -magnitude : magnitude;
}

float3 from_srgb(float3 c) {
    return select(c <= 0.04045, c / 12.92, pow((c + 0.055) / 1.055, 2.4));
}

// Octahedral, the lab's y folded.
float3 oct_decode(float2 e) {
    float3 n = float3(e.x, 1.0 - abs(e.x) - abs(e.y), e.y);
    const float t = max(-n.y, 0.0);
    n.x -= (n.x >= 0.0 ? 1.0 : -1.0) * t;
    n.z -= (n.z >= 0.0 ? 1.0 : -1.0) * t;
    return normalize(n);
}

float3 colour_of(float4 stored) {
    return from_srgb(stored.rgb) * leaf_shape.colour_range.x;
}

float3 dequantized(uint2 words) {
    const float3 q = float3(words.x & 0xffffu, words.x >> 16u, words.y & 0xffffu) / 65535.0;
    return push.lo_target.xyz + (q * (push.hi_dims.xyz - push.lo_target.xyz));
}

// The sun as the tree's own frame has it, for its baked visibility.
float own_light(float4 stored) {
    const float3 sun = normalize(float3(dot(push.x_tint.xyz, frame.sun.xyz),
                                        dot(push.y_translucency.xyz, frame.sun.xyz),
                                        dot(push.z_cutoff.xyz, frame.sun.xyz)));
    return saturate(stored.x + dot(stored.yzw - 0.5, sun));
}

Surface placed(float3 local, float3 normal, float3 colour, float2 uv, float lit) {
    const float3 x = push.x_tint.xyz;
    const float3 y = push.y_translucency.xyz;
    const float3 z = push.z_cutoff.xyz;
    Surface surface;
    surface.from_eye = push.foot_pixel.xyz + (x * local.x) + (y * local.y) + (z * local.z);
    surface.normal = (x * normal.x) + (y * normal.y) + (z * normal.z);
    surface.colour = colour * (1.0 + (0.12 * push.x_tint.w));
    surface.uv = uv;
    surface.lit = lit;
    surface.position = mul(frame.clip_from_eye, float4(surface.from_eye, 1.0));
    return surface;
}

// A kept piece grows about its pivot to hold its share of the crown's area (lod.kept).
float grown(float size) {
    const float dims = push.hi_dims.w;
    return dims > 0.0 ? max(1.0, push.foot_pixel.w * push.lo_target.w / size) : 1.0;
}

[shader("vertex")]
Surface vertex_mesh(uint index : SV_VertexID) {
    const uint at = index * 8u;
    const float3 position = dequantized(uint2(records[at], records[at + 1u]));
    float3 normal = oct_decode(snorm16x2(records[at + 2u]));
    const float2 uv = unorm16x2(records[at + 3u]) * push.uv_calm.xy;
    float3 colour = colour_of(unorm8x4(records[at + 4u]));
    const float4 sun = unorm8x4(records[at + 6u]);
    const uint piece = records[at + 7u] * 8u;
    const float3 pivot = dequantized(uint2(pieces[piece], pieces[piece + 1u]));
    const float size = from_half(pieces[piece + 2u] & 0xffffu);
    const float scale = grown(size);
    if (push.uv_calm.z > 0.0 && scale > 1.0) {
        // Fewer, larger pieces hold more of the differences in a pixel (lod._calmed).
        const float calm = sqrt(pow(scale, -push.hi_dims.w));
        const float3 mean_colour = colour_of(unorm8x4(pieces[piece + 3u]));
        const uint mean_normal = pieces[piece + 5u];
        const float3 towards =
            oct_decode(float2(snorm8(mean_normal & 0xffu), snorm8((mean_normal >> 8u) & 0xffu)));
        colour = mean_colour + ((colour - mean_colour) * calm);
        normal = normalize(towards + ((normal - towards) * calm));
    }
    return placed(pivot + ((position - pivot) * scale), normal, colour, uv, own_light(sun));
}

float4 quaternion(uint word) {
    return float4(snorm8(word & 0xffu), snorm8((word >> 8u) & 0xffu),
                  snorm8((word >> 16u) & 0xffu), snorm8(word >> 24u));
}

float3 rotated(float4 q, float3 v) {
    q = normalize(q);
    const float3 t = 2.0 * cross(q.xyz, v);
    return v + (q.w * t) + cross(q.xyz, t);
}

[shader("vertex")]
Surface vertex_leaf(uint index : SV_VertexID, uint leaf : SV_InstanceID) {
    const uint at = leaf * 10u;
    const uint corner = leaf_shape.indices[index / 4u][index % 4u];
    const float3 base = dequantized(uint2(records[at], records[at + 1u]));
    const float length = from_half(records[at + 1u] >> 16u);
    const float4 rotation = quaternion(records[at + 2u]);
    const uint flags = records[at + 4u] >> 24u;
    float3 offset = leaf_shape.corners[corner].xyz;
    if ((flags & 1u) != 0u) {
        offset.z = 0.0;
    }
    const float scale = grown(length);
    float3 normal = oct_decode(float2(snorm8((records[at + 3u] >> 16u) & 0xffu),
                                      snorm8(records[at + 3u] >> 24u)));
    float3 colour = colour_of(float4(unorm8x4(records[at + 4u]).rgb, 1.0));
    if (scale > 1.0) {
        const float calm = sqrt(pow(scale, -push.hi_dims.w));
        const float3 mean_colour = colour_of(unorm8x4(records[at + 7u]));
        const uint mean_normal = records[at + 9u];
        const float3 towards =
            oct_decode(float2(snorm8(mean_normal & 0xffu), snorm8((mean_normal >> 8u) & 0xffu)));
        colour = mean_colour + ((colour - mean_colour) * calm);
        normal = normalize(towards + ((normal - towards) * calm));
    }
    const float4 uvs = leaf_shape.uvs[corner / 2u];
    const float2 uv = (corner % 2u) == 0u ? uvs.xy : uvs.zw;
    return placed(base + rotated(rotation, offset * length * scale), normal, colour, uv,
                  own_light(unorm8x4(records[at + 6u])));
}

// Lit by sunlight through the air and the cloud, let through the tree's own leaves, and the sky's
// light; through a leaf from behind; seen through the air between and tone mapped.
[shader("fragment")]
float4 fragment_main(Surface surface) : SV_Target {
    const float4 texel = albedo.Sample(surface.uv);
    const float translucency = push.y_translucency.w;
    const bool bark = push.uv_calm.w > 0.0;
    if (!bark && texel.a < push.z_cutoff.w) {
        discard;
    }
    float3 normal = normalize(surface.normal);
    const float3 to_eye = -normalize(surface.from_eye);
    if (translucency > 0.0 && dot(normal, to_eye) < 0.0) {
        normal = -normal;
    }
    const float3 up = normalize(push.y_translucency.xyz);
    const float3 sun = frame.sun.xyz;
    const float scale = lighting.sky_scale.w;
    const float3 clear = transmittance(air, transmittance_table, lighting.up_height.w, dot(up, sun));
    const Shade shade = shade_at(lighting, cloud_shadow, surface.from_eye);
    const float3 sunlight = through_cloud(shade, clear) * surface.lit;
    const float3 sky = sky_under_cloud(lighting, shade, clear * saturate(dot(up, sun)));
    const float facing = dot(normal, sun);
    // Light through a leaf, strongest looking toward the sun through it, tinted yellow-green.
    const float through = translucency * max(-facing, 0.0) *
                          (0.4 + (0.6 * pow(max(dot(-to_eye, sun), 0.0), 3.0)));
    const float cavity = bark ? lerp(0.45, 1.0, smoothstep(0.0, 0.8, texel.a)) : 1.0;
    const float3 base = surface.colour * texel.rgb * cavity;
    const float3 lit = (sunlight * max(facing, 0.0)) +
                       (sunlight * through * float3(1.25, 1.35, 0.6)) +
                       (sky * (0.5 + (0.5 * dot(normal, up))));
    const float3 radiance = base * lit / pi;

    const float4 clip = mul(frame.clip_from_eye, float4(surface.from_eye, 1.0));
    const float2 uv = ((clip.xy / clip.w) * 0.5) + 0.5;
    const float4 between =
        aerial_perspective(aerial_volume, uv, length(surface.from_eye) * scale, lighting.reach.x);
    return float4(tone_mapped((radiance * between.a) + between.rgb, lighting.sun_exposure.w), 1.0);
}
