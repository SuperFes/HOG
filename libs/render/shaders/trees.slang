import atmosphere;
import bend;
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
    // xy the uvs' scale; z 1 where pieces are calmed toward their neighbours' means; w how its
    // textures are read: 0 leaves, 1 bark (the albedo's alpha its height), 2 branch cards (the
    // second texture their normals and leafiness).
    float4 uv_calm;
    // x the share of its pixels handed between levels; y 1 drawn as the level taking them.
    float4 handing;
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
[[vk::binding(16, 0)]] Sampler2D second_map;

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

// A threshold, 0 to 1, for a pixel: interleaved gradient noise (Jimenez 2014), even enough that
// a share of a tree's pixels handed between levels reads as a fade rather than a pattern.
float handing_noise(float2 pixel) {
    return frac(52.9829189 * frac(dot(floor(pixel), float2(0.06711056, 0.00583715))));
}

// Whether a pixel stays with this level: of the share `handed` to the next level, those under
// the threshold go, and the next level keeps just those.
bool kept(float2 pixel, float handed, bool incoming) {
    return (handing_noise(pixel) < handed) == incoming;
}

// As hog::render::Trees' handed_at: the share handed over `away` metres off, at `at`.
float handed_at(float away, float at) {
    return smoothstep(0.9 * at, 1.1 * at, away);
}

// What the sky gives a surface facing `facing_up` (its normal's cosine with up): the sky over it,
// and facing down, the sky's light thrown back off the ground and the leaves round it; a leaf
// lets through some of what falls on its other side.
float3 ambient_of(float3 sky, float facing_up, float translucency) {
    static const float bounced = 0.35;
    const float over = lerp(bounced, 1.0, (0.5 * facing_up) + 0.5);
    return sky * (over + (translucency * 0.5 * max(-facing_up, 0.0)));
}

// Lit by sunlight through the air and the cloud, let through the tree's own leaves, and the sky's
// light; through a leaf from behind; seen through the air between and tone mapped.
[shader("fragment")]
float4 fragment_main(Surface surface) : SV_Target {
    if (!kept(surface.position.xy, push.handing.x, push.handing.y > 0.5)) {
        discard;
    }
    const float4 texel = albedo.Sample(surface.uv);
    const bool bark = push.uv_calm.w > 0.5 && push.uv_calm.w < 1.5;
    const bool card = push.uv_calm.w > 1.5;
    if (!bark && texel.a < push.z_cutoff.w) {
        discard;
    }
    float translucency = push.y_translucency.w;
    float3 normal = normalize(surface.normal);
    if (card) {
        // Each texel's leaves' own way, in the tree's frame, and how leafy it is.
        const float4 baked = second_map.Sample(surface.uv);
        const float3 own = (baked.rgb * 2.0) - 1.0;
        normal = normalize((push.x_tint.xyz * own.x) + (push.y_translucency.xyz * own.y) +
                           (push.z_cutoff.xyz * own.z));
        translucency *= baked.a;
    }
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
                       ambient_of(sky, dot(normal, up), translucency);
    const float3 radiance = base * lit / pi;

    const float4 clip = mul(frame.clip_from_eye, float4(surface.from_eye, 1.0));
    const float2 uv = ((clip.xy / clip.w) * 0.5) + 0.5;
    const float4 between =
        aerial_perspective(aerial_volume, uv, length(surface.from_eye) * scale, lighting.reach.x);
    return float4(tone_mapped((radiance * between.a) + between.rgb, lighting.sun_exposure.w), 1.0);
}

// Far trees as the impostors they were baked as (prototypes/trees/impostor.py): each a square
// facing the eye, through which the four views nearest the eye's way are stepped to where their
// depth puts the leaves, and lit as the leaves under each texel would be.

// As hog::render::Trees' Instance: the foot from the trees' origin, and the tree's own x and y
// (up) axes scaled by its size; w: its tint, its hash.
struct Instance {
    float4 foot_tint;
    float4 x_hash;
    float4 y;
};

// As hog::render::Trees' FarPush.
struct FarPush {
    // xyz the trees' origin from the eye; w the first instance drawn.
    float4 origin_first;
    float4 up_height; // the bend's
    // The bend's radii; metres from the eye impostors are drawn from and to.
    float4 radii_reach;
    // The impostor's middle and radius, in the tree's own frame.
    float4 centre_radius;
    // x views a side; y the leaves' cutoff.
    float4 views_cutoff;
};

[[vk::push_constant]] ConstantBuffer<FarPush> far;

[[vk::binding(10, 0)]] StructuredBuffer<Instance> instances;
[[vk::binding(11, 0)]] Sampler2D atlas_albedo;
[[vk::binding(12, 0)]] Sampler2D atlas_normal;
[[vk::binding(13, 0)]] Sampler2D atlas_depth;
[[vk::binding(14, 0)]] Sampler2D atlas_sunvis;
[[vk::binding(15, 0)]] Sampler2D atlas_moment;

struct Seen {
    float4 position : SV_Position;
    float3 from_eye;
    // The tree's own axes, x, y and z, scaled by its size, and its foot, from the eye.
    nointerpolation float3 x;
    nointerpolation float3 y;
    nointerpolation float3 z;
    nointerpolation float3 foot;
    nointerpolation float tint;
    // Of its pixels, the share taken from the cards, and the share given up to nothing far off.
    nointerpolation float taken;
    nointerpolation float given;
};

[shader("vertex")]
Seen vertex_impostor(uint index : SV_VertexID, uint instance : SV_InstanceID) {
    static const float2 corners[6] = {float2(-1.0, -1.0), float2(1.0, -1.0), float2(1.0, 1.0),
                                      float2(-1.0, -1.0), float2(1.0, 1.0), float2(-1.0, 1.0)};
    const Instance tree = instances[uint(far.origin_first.w) + instance];
    const float3 unbent = far.origin_first.xyz + tree.foot_tint.xyz;
    Seen seen;
    seen.foot = bent(unbent, far.up_height, far.radii_reach.xy);
    seen.x = tree.x_hash.xyz;
    seen.y = tree.y.xyz;
    seen.z = cross(seen.x, seen.y) / length(seen.x);
    seen.tint = tree.foot_tint.w;
    const float scale = length(seen.x);
    const float3 local = far.centre_radius.xyz;
    const float3 centre =
        seen.foot + (seen.x * local.x) + (seen.y * local.y) + (seen.z * local.z);
    const float away = length(seen.foot);
    const float radius = far.centre_radius.w * scale;
    const float3 look = centre / max(length(centre), 1e-3);
    const float3 right = normalize(cross(look, normalize(seen.y)));
    const float3 up = cross(right, look);
    // At the front of the ball holding the tree, grown to still cover it, so every leaf lies
    // behind the square and its depth only moves away: the depth test can then cull before shading.
    const float before = max(length(centre) - radius, 0.5 * length(centre));
    const float grow = before / max(length(centre), 1e-3);
    seen.from_eye = (look * before) +
                    (((right * corners[index].x) + (up * corners[index].y)) * radius * grow);
    seen.position = mul(frame.clip_from_eye, float4(seen.from_eye, 1.0));
    seen.taken = handed_at(away, far.radii_reach.z);
    seen.given = handed_at(away, far.radii_reach.w);
    // Wholly the cards' or wholly gone, nothing.
    if (seen.taken <= 0.0 || seen.given >= 1.0) {
        seen.position = float4(0.0, 0.0, -1.0, 1.0);
    }
    return seen;
}

uint views() {
    return uint(far.views_cutoff.x);
}

float3 decode_view(float2 g) {
    const float2 uv = (((g + 0.5) / float(views())) * 2.0) - 1.0;
    const float2 p = float2(uv.x + uv.y, uv.x - uv.y) * 0.5;
    return normalize(float3(p.x, 1.0 - abs(p.x) - abs(p.y), p.y));
}

float2 encode_view(float3 d) {
    d.y = max(d.y, 0.0);
    d /= abs(d.x) + abs(d.y) + abs(d.z);
    const float2 uv = float2(d.x + d.z, d.x - d.z);
    return (((uv * 0.5) + 0.5) * float(views())) - 0.5;
}

struct View {
    float4 albedo;
    float4 normal;
    float4 sun;
    float4 moment;
    float yz;
    float3 hit;
};

// One baked view where the ray from `ro` along `rd`, in the tree's own frame, meets the tree as
// that view saw it: stepping to the height its depth gives, so the views agree where each branch
// is instead of ghosting it.
View sample_view(float2 g, float3 ro, float3 rd) {
    const float3 d = decode_view(g);
    const float3 a = abs(d.y) < 0.99 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
    const float3 r = normalize(cross(a, d));
    const float3 u = cross(d, r);
    const float3 centre = far.centre_radius.xyz;
    const float radius = far.centre_radius.w;
    float across = dot(rd, d);
    across = abs(across) < 0.05 ? (across < 0.0 ? -0.05 : 0.05) : across;
    float h = 0.0;
    float2 cell = float2(0.0);
    View view;
    for (int step = 0; step < 3; ++step) {
        view.hit = ro + (rd * ((h - dot(ro - centre, d)) / across));
        const float2 st =
            (float2(dot(view.hit - centre, r), dot(view.hit - centre, u)) / radius * 0.5) + 0.5;
        cell = (g + clamp(st, 0.002, 0.998)) / float(views());
        h = radius - (atlas_depth.Sample(cell).r * 2.0 * radius);
    }
    view.hit = ro + (rd * ((h - dot(ro - centre, d)) / across));
    view.albedo = atlas_albedo.Sample(cell);
    view.normal = atlas_normal.Sample(cell);
    view.sun = atlas_sunvis.Sample(cell);
    view.moment = atlas_moment.Sample(cell);
    view.yz = atlas_depth.Sample(cell).g - 0.5;
    return view;
}

// Abramowitz & Stegun 7.1.26, within 1.5e-7.
float erf_approx(float x) {
    const float t = 1.0 / (1.0 + (0.3275911 * abs(x)));
    const float y =
        1.0 - (t * (0.254829592 +
                    (t * (-0.284496736 + (t * (1.421413741 + (t * (-1.453152027 +
                                                                  (t * 1.061405429))))))))) *
                  exp(-x * x);
    return sign(x) * y;
}

// The mean of max(t, 0) for t with mean mu and spread sd, taken as normal.
float clamped_mean(float mu, float sd) {
    const float z = mu / sd;
    return (mu * 0.5 * (1.0 + erf_approx(z * 0.70710678))) +
           (sd * 0.39894228 * exp(-0.5 * z * z));
}

// A texel's leaves' mean sunlit and lit-from-behind shares, from their mean normal and mean
// n n^T, which average linearly so any mip lights as the leaves under it do.
float2 leaf_light(float3 mean_n, float4 m, float yz, float3 l) {
    const float zz = 1.0 - m.x - m.y;
    const float lml = (m.x * l.x * l.x) + (m.y * l.y * l.y) + (zz * l.z * l.z) +
                      (2.0 * ((m.z * l.x * l.y) + (m.w * l.x * l.z) + (yz * l.y * l.z)));
    const float mu = dot(mean_n, l);
    const float even = 0.25 + (0.15625 * ((3.0 * lml) - 1.0));
    const float sd = sqrt(max(lml - (mu * mu), 1e-8));
    const float narrow = pow(saturate(length(mean_n)), 8.0);
    return float2(lerp(max(even + (0.5 * mu), 0.0), clamped_mean(mu, sd), narrow),
                  lerp(max(even - (0.5 * mu), 0.0), clamped_mean(-mu, sd), narrow));
}

// A highlight of sharpness s averaged over a texel's leaves, widened and dimmed by their spread.
float highlight(float nh, float agree, float s) {
    const float ft = agree / max(agree + (s * (1.0 - agree)), 1e-4);
    return pow(max(nh, 0.0), ft * s) * (1.0 + (ft * s)) / (1.0 + s);
}

struct Lit {
    float4 colour : SV_Target;
    // Depth is reversed, so further is less.
    float depth : SV_DepthLessEqual;
};

[shader("fragment")]
Lit fragment_impostor(Seen seen) {
    if (!kept(seen.position.xy, seen.taken, true) || !kept(seen.position.xy, seen.given, false)) {
        discard;
    }
    const float scale = length(seen.x);
    const float3 x = seen.x / scale;
    const float3 y = seen.y / scale;
    const float3 z = seen.z / scale;
    const float3x3 to_own = float3x3(x, y, z);
    // The eye and the ray in the tree's own frame.
    const float3 ro = mul(to_own, -seen.foot) / scale;
    const float3 rd = normalize(mul(to_own, seen.from_eye - seen.foot) / scale - ro);
    const float3 sun_own = normalize(mul(to_own, frame.sun.xyz));
    const float3 look = normalize(ro - far.centre_radius.xyz);
    const float2 g = encode_view(look);
    const float last = float(views()) - 1.0;
    const float2 g0 = clamp(floor(g), 0.0, last);
    const float2 f = saturate(g - g0);
    View seen_views[4];
    float shares[4];
    float covered = 0.0;
    float weights = 0.0;
    float3 hit = float3(0.0);
    for (int j = 0; j < 2; ++j) {
        for (int i = 0; i < 2; ++i) {
            const int k = (j * 2) + i;
            const float w = (i == 0 ? 1.0 - f.x : f.x) * (j == 0 ? 1.0 - f.y : f.y);
            seen_views[k] = sample_view(min(g0 + float2(i, j), last), ro, rd);
            shares[k] = w * seen_views[k].albedo.a;
            covered += shares[k];
            hit += seen_views[k].hit * shares[k];
            weights += w;
        }
    }
    const float coverage = covered / max(weights, 1e-4);
    if (coverage < far.views_cutoff.y) {
        discard;
    }
    hit /= max(covered, 1e-4);
    const float3 world = seen.foot + (seen.x * hit.x) + (seen.y * hit.y) + (seen.z * hit.z);

    const float3 up = y;
    const float sky_scale = lighting.sky_scale.w;
    const float3 sun = frame.sun.xyz;
    const float3 clear = transmittance(air, transmittance_table, lighting.up_height.w, dot(up, sun));
    const Shade shade = shade_at(lighting, cloud_shadow, world);
    const float3 sunlight = through_cloud(shade, clear);
    const float3 sky = sky_under_cloud(lighting, shade, clear * saturate(dot(up, sun)));
    const float3 to_eye = -normalize(world);
    const float3 to_eye_own = normalize(mul(to_own, to_eye));
    const float toward_sun = 0.4 + (0.6 * pow(max(dot(-to_eye, sun), 0.0), 3.0));
    const float4 clip = mul(frame.clip_from_eye, float4(world, 1.0));
    const float2 uv = ((clip.xy / clip.w) * 0.5) + 0.5;
    const float4 between =
        aerial_perspective(aerial_volume, uv, length(world) * sky_scale, lighting.reach.x);
    const float exposure = lighting.sun_exposure.w;
    float3 colour = float3(0.0);
    for (int k = 0; k < 4; ++k) {
        const View view = seen_views[k];
        const float3 base = view.albedo.rgb * (1.0 + (0.12 * seen.tint));
        const float3 mean_n = (view.normal.rgb * 2.0) - 1.0;
        const float agree = saturate(length(mean_n));
        const float3 normal = mean_n / max(length(mean_n), 1e-4);
        const float4 m = float4(view.moment.xy, view.moment.zw - 0.5);
        const float2 lit = leaf_light(mean_n, m, view.yz, sun_own);
        const float through = 0.55 * view.normal.a * lit.y * toward_sun;
        const float own = saturate(view.sun.x + dot(view.sun.yzw - 0.5, sun_own));
        const float3 ambient =
            ambient_of(sky, lerp(0.0, normal.y, agree), 0.55 * view.normal.a);
        const float3 h = normalize(sun_own + to_eye_own);
        const float3 dark = base * ambient / pi;
        const float3 sunny = dark + (base * sunlight * lit.x / pi) +
                             (base * float3(1.25, 1.35, 0.6) * sunlight * through / pi) +
                             (sunlight * 0.04 * highlight(dot(normal, h), agree, 24.0) / pi);
        // A texel stands for many leaves, a share in sun and the rest in shade: each is toned on
        // its own, as a pixel showing them would be.
        colour += ((tone_mapped((sunny * between.a) + between.rgb, exposure) * own) +
                   (tone_mapped((dark * between.a) + between.rgb, exposure) * (1.0 - own))) *
                  shares[k];
    }
    Lit out;
    out.colour = float4(colour / max(covered, 1e-4), 1.0);
    out.depth = clip.z / clip.w;
    return out;
}
