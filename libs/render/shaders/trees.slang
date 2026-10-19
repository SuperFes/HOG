import atmosphere;
import bend;
import shadow;
import tone;

// Pine stamps, each drawn from a tree anchor bound to its spot in the world: three crossed quads,
// a mask cut in the fragment shader shaping a tiered conical crown of boughs over a bare trunk.
// The anchors are shared with the grown forest, which covers the near ground; the stamps hold off
// inside its reach and carry on beyond, shrinking away at the edge of their own.

struct Push {
    float4x4 clip_from_eye;
    // xyz up through the eye, w its height above sea level.
    float4 bend_up;
    // x the planet's radius and y the faked one drawing bends it onto; z seconds of sway so far,
    // w the radians across a pixel.
    float4 bend_radii;
    // xyz the wind near the ground, m/s; w unused.
    float4 wind;
    // xyz the tile's origin from the eye, in metres; w (as bits) the tile's first anchor drawn.
    float4 origin;
};

[[vk::push_constant]] ConstantBuffer<Push> push;

struct Settings {
    // x metres the stamps hold off while the grown forest covers; y metres from the eye they
    // have shrunk away by.
    float4 reach;
};

[[vk::binding(0, 0)]] ConstantBuffer<Settings> settings;
// The tile's tree anchors, as render::TreeAnchor lies.
[[vk::binding(1, 0)]] ByteAddressBuffer anchors;
[[vk::binding(2, 0)]] ConstantBuffer<Air> air;
[[vk::binding(3, 0)]] ConstantBuffer<Lighting> lighting;
[[vk::binding(4, 0)]] Sampler2D transmittance_table;
[[vk::binding(5, 0)]] Sampler2DArray aerial_volume;
// As cloud_march.slang's.
[[vk::binding(6, 0)]] Sampler2D cloud_shadow;

static const uint anchor_stride = 48; // bytes, as render::TreeAnchor lies

// Bare wood, for the trunk, as the grown forest's bark.
static const float3 wood = float3(0.055, 0.045, 0.038);

uint hashed(uint at) {
    at ^= at >> 16;
    at *= 0x7feb352du;
    at ^= at >> 15;
    at *= 0x846ca68bu;
    at ^= at >> 16;
    return at;
}

float unit(uint at) {
    return float(hashed(at) & 0xffffu) / 65536.0;
}

// The two triangles of a quad: bottom left and right, then top left and right.
static const uint corners[6] = { 0, 1, 2, 2, 1, 3 };

struct Tree {
    float4 position : SV_Position;
    float3 colour;
    float3 normal;
    float3 from_eye;
    float3 drawn; // from the eye, bent
    float height; // above sea level
    float2 uv;    // -0.5 to 0.5 across the quad, 0 to 1 up it
    float pixels; // the quad's width in pixels, for shedding detail far off
    nointerpolation uint seed; // the tree's own, keyed to its lattice cell
};

// Nothing: a point's triangles cover no pixels.
Tree nothing() {
    Tree gone;
    gone.position = float4(0.0, 0.0, 0.0, 1.0);
    gone.colour = float3(0.0);
    gone.normal = float3(0.0, 0.0, 1.0);
    gone.from_eye = float3(0.0);
    gone.drawn = float3(0.0);
    gone.height = 0.0;
    gone.uv = float2(0.0);
    gone.pixels = 0.0;
    gone.seed = 0;
    return gone;
}

[shader("vertex")]
Tree vertex_main(uint vertex: SV_VertexID, uint instance: SV_InstanceID) {
    const uint at = (asuint(push.origin.w) + instance) * anchor_stride;
    const float3 root = push.origin.xyz + asfloat(anchors.Load3(at));
    const float grown = asfloat(anchors.Load(at + 12));
    const float3 normal = asfloat(anchors.Load3(at + 16));
    const uint hash = anchors.Load(at + 28);
    const float3 crown = asfloat(anchors.Load3(at + 32));

    const float away = length(root);
    // Held off while the grown forest covers; full sized until near the edge of their reach,
    // then shrinking away.
    const float fade = saturate((settings.reach.y - away) / (0.2 * settings.reach.y));
    if (away < settings.reach.x || fade <= 0.0) {
        return nothing();
    }
    const float above = height_above_sea(root, push.bend_up, push.bend_radii.x);
    const float height = grown * fade;
    const float width = height * (0.42 + (0.12 * unit(hash + 5u)));

    // Three quads crossed about the tree's own up: near vertical, with a shade of the ground's
    // lean, along the very same axis the grown forest's trees stand. The mask does the shaping.
    const float3 upright = normalize(normal + (push.bend_up.xyz * 4.0));
    const uint quad = vertex / 6;
    const uint corner = corners[vertex % 6];
    const float3 beside = normalize(abs(upright.z) < 0.9 ? cross(upright, float3(0.0, 0.0, 1.0))
                                                         : cross(upright, float3(1.0, 0.0, 0.0)));
    const float3 ahead = cross(upright, beside);
    const float facing = (float(quad) * 1.0471976) + (6.2831853 * unit(hash + 3u));
    const float3 width_dir = (beside * cos(facing)) + (ahead * sin(facing));

    const float side_x = (corner & 1u) == 0 ? -0.5 : 0.5;
    const float t = corner < 2 ? 0.0 : 1.0;
    // Trunks hold fast; the crown's top sways, held over by strong wind.
    const float speed = length(push.wind.xyz);
    float3 sway = float3(0.0);
    if (speed > 1e-3) {
        const float3 along = push.wind.xyz / speed;
        const float held = (speed * speed) / ((speed * speed) + 320.0);
        const float rock =
            0.025 * sin((push.bend_radii.z * (0.9 + (0.15 * speed))) + (6.2831853 * unit(hash + 6u)));
        sway = along * (height * ((held * 0.16) + (rock * saturate(speed / 8.0))));
    }
    const float3 placed =
        root + (width_dir * (side_x * width)) + (upright * (height * t)) + (sway * (t * t * t));

    Tree tree;
    tree.from_eye = placed;
    tree.drawn = bent(placed, push.bend_up, push.bend_radii.xy);
    tree.position = mul(push.clip_from_eye, float4(tree.drawn, 1.0));
    // Rounded off per corner, so the flat quads shade like a volume.
    tree.normal =
        normalize(upright + (width_dir * (side_x * 0.9)) + (upright * ((t - 0.35) * 0.5)));
    tree.height = above;
    tree.pixels = width / max(push.bend_radii.w * away, 1e-6);
    tree.colour = crown * (0.75 + (0.45 * unit(hash + 7u))) * (0.75 + (0.25 * t));
    tree.uv = float2(side_x, t);
    tree.seed = hash;
    return tree;
}

static const uint air_part = 0;
static const uint needles = 1;
static const uint bark = 2;

struct Cut {
    uint part;
    float shade;
};

// What of the pine the point falls on; the crown sheds its tiers once the whole quad is only a
// few pixels across.
Cut pine_cut(uint seed, float2 uv, float pixels) {
    Cut cut;
    cut.part = air_part;
    cut.shade = 1.0;
    if (pixels < 7.0) {
        // Far off, a plain wedge on a one-pixel stem.
        const float t = saturate((uv.y - 0.06) / 0.94);
        if (uv.y >= 0.06 && abs(uv.x) < (0.38 * (1.0 - t)) + 0.012) {
            cut.part = needles;
        } else if (abs(uv.x) < 0.03 && uv.y < 0.5) {
            cut.part = bark;
        }
        return cut;
    }
    const float foot = 0.12 + (0.08 * unit(seed + 11u)); // where the crown begins
    const float phase = unit(seed + 12u);
    const float tiers = 5.0 + (2.0 * unit(seed + 13u));
    if (uv.y >= foot) {
        const float t = (uv.y - foot) / (1.0 - foot);
        // Each tier of boughs widest at its own bottom, drawn in toward its top, the whole
        // crown narrowing to the leader; a ragged hem of needles on every edge.
        const float along = frac((t * tiers) + phase);
        const float step = lerp(1.0, 0.5, along);
        const float spread = pow(1.0 - t, 0.85);
        const float ragged = 0.02 * sin((uv.y * 47.0) + (6.2831853 * phase)) * spread;
        const float half_width = (0.42 * spread * step) + 0.012 + ragged;
        if (abs(uv.x) < half_width) {
            cut.part = needles;
            // Darkest under a bough, lit along its top; deeper toward the trunk.
            cut.shade = lerp(0.55, 1.1, saturate(along * 1.6)) *
                        (0.8 + (0.25 * saturate(abs(uv.x) / max(half_width, 0.02))));
            return cut;
        }
    }
    const float trunk = (0.03 * (1.0 - (0.55 * uv.y))) + 0.004;
    if (abs(uv.x) < trunk && uv.y < 0.9) {
        cut.part = bark;
    }
    return cut;
}

// Lit as the ground beneath it, as grass.slang lights its blades.
[shader("fragment")]
float4 fragment_main(Tree tree) : SV_Target {
    const Cut cut = pine_cut(tree.seed, tree.uv, tree.pixels);
    if (cut.part == air_part) {
        discard;
    }
    const float3 albedo = cut.part == bark ? wood : tree.colour * cut.shade;
    const float3 sun = lighting.sun_exposure.xyz;
    const float scale = lighting.sky_scale.w;
    const float3 normal = normalize(tree.normal);
    const float3 up = normalize(tree.from_eye +
                                (push.bend_up.xyz * (push.bend_radii.x + push.bend_up.w)));
    const float3 clear =
        transmittance(air, transmittance_table, max(tree.height, 0.0) * scale, dot(up, sun));
    const Shade shade = shade_at(lighting, cloud_shadow, tree.drawn);
    const float3 sunlight = through_cloud(shade, clear);
    const float3 sky = sky_under_cloud(lighting, shade, clear * saturate(dot(up, sun)));
    const float3 lit =
        (sunlight * saturate(dot(normal, sun))) + (sky * (0.5 + (0.5 * dot(normal, up))));
    const float3 radiance = albedo * lit / pi;

    const float4 clip = mul(push.clip_from_eye, float4(tree.drawn, 1.0));
    const float2 uv = ((clip.xy / clip.w) * 0.5) + 0.5;
    const float4 between =
        aerial_perspective(aerial_volume, uv, length(tree.drawn) * scale, lighting.reach.x);
    return float4(tone_mapped((radiance * between.a) + between.rgb, lighting.sun_exposure.w), 1.0);
}
