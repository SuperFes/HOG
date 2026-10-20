import atmosphere;
import bend;
import season;
import shadow;
import tone;

// Tree stamps, each drawn from a tree anchor bound to its spot in the world: three crossed quads, a
// mask cut in the fragment shader shaping the tree's crown over a bare trunk — a pine's tiered cone
// of boughs, an oak's lumpy round head of leaves, a maple's taller smoother oval, a palm's arching
// fronds high on a bare stem.
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

static const uint genus_count = 4;

struct Settings {
    // x metres the stamps hold off while the grown forest covers; y metres from the eye they
    // have shrunk away by.
    float4 reach;
    // Each genus' year, as render::GenusSeasons has it.
    float4 leafing[genus_count];
    float4 first[genus_count];
    float4 last[genus_count];
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

static const uint anchor_stride = 40; // bytes, as render::TreeAnchor lies

// Bare wood, for the trunk, as the grown forest's bark: pine, oak, maple, then palm.
static const float3 woods[genus_count] = { float3(0.055, 0.045, 0.038), float3(0.07, 0.064, 0.056),
                                           float3(0.08, 0.075, 0.068), float3(0.085, 0.072, 0.055) };
static const uint pine = 0;
static const uint oak = 1;
static const uint palm = 3;

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

// As render::foliage_of unpacks them: square-rooted bytes, squared back.
float3 foliage_of(uint packed) {
    const float3 rooted = float3(float(packed & 0xffu), float((packed >> 8) & 0xffu),
                                 float((packed >> 16) & 0xffu)) / 255.0;
    return rooted * rooted;
}

// As render::genus_of: the last byte, anything unknown a pine.
uint genus_of(uint packed) {
    const uint genus = packed >> 24;
    return genus < genus_count ? genus : 0u;
}

// As flora::aged() scales a species: `young` for a sapling, `old` at the end, 1 in its prime. The
// stamps' factors follow each species' own ageing.
static const float prime_age = 0.7;
float across(float age, float young, float old) {
    return age >= prime_age ? lerp(1.0, old, (age - prime_age) / (1.0 - prime_age))
                            : lerp(young, 1.0, age / prime_age);
}

// How far a palm's fronds reach, over its height: further the younger it is, its stem not yet grown
// up under them, as flora::palm() ages.
float palm_reach(uint seed, float age) {
    return (0.3 + (0.05 * unit(seed + 5u))) * across(age, 1.7, 0.95);
}

// The two triangles of a quad: bottom left and right, then top left and right.
static const uint corners[6] = { 0, 1, 2, 2, 1, 3 };

struct Tree {
    float4 position : SV_Position;
    float3 colour; // the crown's green
    float tone;    // the tree's own shade of it, darker low in the crown
    float3 normal;
    float3 from_eye;
    float3 drawn; // from the eye, bent
    float height; // above sea level
    float2 uv;    // -0.5 to 0.5 across the quad, 0 to 1 up it
    float pixels; // the quad's width in pixels, for shedding detail far off
    nointerpolation uint seed; // the tree's own, keyed to its lattice cell
    nointerpolation float age; // 0 a sapling, 1 old
    nointerpolation float own; // how early it keeps its kind's dates, as season.slang's
    nointerpolation uint genus;
};

// Nothing: a point's triangles cover no pixels.
Tree nothing() {
    Tree gone;
    gone.position = float4(0.0, 0.0, 0.0, 1.0);
    gone.colour = float3(0.0);
    gone.tone = 0.0;
    gone.normal = float3(0.0, 0.0, 1.0);
    gone.from_eye = float3(0.0);
    gone.drawn = float3(0.0);
    gone.height = 0.0;
    gone.uv = float2(0.0);
    gone.age = 0.0;
    gone.pixels = 0.0;
    gone.own = 0.0;
    gone.seed = 0;
    gone.genus = 0;
    return gone;
}

[shader("vertex")]
Tree vertex_main(uint vertex: SV_VertexID, uint instance: SV_InstanceID) {
    const uint at = (asuint(push.origin.w) + instance) * anchor_stride;
    const float3 root = push.origin.xyz + asfloat(anchors.Load3(at));
    const float grown = asfloat(anchors.Load(at + 12));
    const float3 normal = asfloat(anchors.Load3(at + 16));
    const uint hash = anchors.Load(at + 28);
    const uint packed = anchors.Load(at + 32);
    const float3 crown = foliage_of(packed);
    const uint genus = genus_of(packed);
    const float age = saturate(asfloat(anchors.Load(at + 36)));

    const float away = length(root);
    // Held off while the grown forest covers; full sized until near the edge of their reach,
    // then shrinking away.
    const float fade = saturate((settings.reach.y - away) / (0.2 * settings.reach.y));
    if (away < settings.reach.x || fade <= 0.0) {
        return nothing();
    }
    const float above = height_above_sea(root, push.bend_up, push.bend_radii.x);
    const float height = grown * fade;
    // Pine saplings and old pines spread wider for their height than one in its prime; the
    // broadleaves spread wider every year, a maple narrower than an oak; a palm's quad holds its
    // fronds' reach either way.
    const float spread = genus == pine   ? (0.42 + (0.12 * unit(hash + 5u))) * across(age, 1.15, 1.2)
                         : genus == oak  ? (0.7 + (0.12 * unit(hash + 5u))) * across(age, 0.85, 1.2)
                         : genus == palm ? 2.2 * palm_reach(hash, age)
                                         : (0.52 + (0.1 * unit(hash + 5u))) * across(age, 0.9, 1.2);
    const float width = height * spread;

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
    tree.colour = crown;
    tree.tone = (0.75 + (0.45 * unit(hash + 7u))) * (0.75 + (0.25 * t));
    tree.own = own_season(unit(hash + 12u), age);
    tree.uv = float2(side_x, t);
    tree.seed = hash;
    tree.age = age;
    tree.genus = genus;
    return tree;
}

static const uint air_part = 0;
static const uint needles = 1;
static const uint bark = 2;

struct Cut {
    uint part;
    float shade;
    float2 patch; // where in the crown's patches of leaves, each turning on its own dates
    bool twig;    // where the crown, bare, shows its branches
};

// What of the pine the point falls on; the crown sheds its tiers once the whole quad is only a
// few pixels across. As the grown forest's: a sapling green to the ground, an old tree's crown
// raised high on its trunk and rounded.
Cut pine_cut(uint seed, float age, float2 uv, float pixels) {
    Cut cut;
    cut.part = air_part;
    cut.shade = 1.0;
    cut.patch = float2(0.0);
    cut.twig = false;
    const float raised = across(age, 0.25, 2.4);
    if (pixels < 7.0) {
        // Far off, a plain wedge on a one-pixel stem.
        const float base = 0.06 * raised;
        const float t = saturate((uv.y - base) / (1.0 - base));
        if (uv.y >= base && abs(uv.x) < (0.38 * (1.0 - t)) + 0.012) {
            cut.part = needles;
        } else if (abs(uv.x) < 0.03 && uv.y < 0.5) {
            cut.part = bark;
        }
        return cut;
    }
    const float foot = (0.12 + (0.08 * unit(seed + 11u))) * raised; // where the crown begins
    const float phase = unit(seed + 12u);
    const float tiers = 5.0 + (2.0 * unit(seed + 13u));
    if (uv.y >= foot) {
        const float t = (uv.y - foot) / (1.0 - foot);
        // Each tier of boughs widest at its own bottom, drawn in toward its top, the whole
        // crown narrowing to the leader; a ragged hem of needles on every edge.
        const float along = frac((t * tiers) + phase);
        const float step = lerp(1.0, 0.5, along);
        const float spread = pow(1.0 - t, lerp(0.85, 0.5, saturate((age - prime_age) / 0.3)));
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

// A broadleaf's head: where it begins up the trunk, how that moves with age, and how lumpy its
// edge.
struct Head {
    float foot;
    float young;
    float old;
    float lumps;
};

// An oak's head starts high and lumpy; a maple's lower, longer and smoother.
Head head_of(uint genus) {
    if (genus == oak) {
        const Head oak_head = { 0.28, 0.6, 1.3, 1.0 };
        return oak_head;
    }
    const Head maple_head = { 0.18, 0.7, 1.2, 0.45 };
    return maple_head;
}

// What of the broadleaf the point falls on: a round head of leaves, lumpy at its edge, on a bare
// trunk; the lumps and mottle go once the whole quad is only a few pixels across. As the grown
// forest's, a sapling's head low on its stem, an old tree's raised.
Cut broadleaf_cut(uint seed, float age, float2 uv, float pixels, Head head) {
    Cut cut;
    cut.part = air_part;
    cut.shade = 1.0;
    cut.patch = float2(0.0);
    cut.twig = false;
    const float foot = (head.foot + (0.08 * unit(seed + 11u))) * across(age, head.young, head.old);
    const float middle = (foot + 1.0) * 0.5;
    const float2 off = float2(uv.x / 0.48, (uv.y - middle) / ((1.0 - foot) * 0.5));
    float reach = length(off);
    const bool near = pixels >= 7.0;
    if (near) {
        const float angle = atan2(off.y, off.x);
        const float phase = 6.2831853 * unit(seed + 12u);
        reach -= head.lumps *
                 ((0.1 * sin((angle * 5.0) + phase)) + (0.06 * sin((angle * 9.0) - (2.0 * phase))));
    }
    if (reach < 1.0) {
        cut.part = needles;
        // Lit over its top, dark beneath, each cluster of leaves its own shade.
        const uint across_at = uint((uv.x + 0.5) * 9.0);
        const uint up_at = uint(uv.y * 14.0);
        const float mottle = near ? 0.85 + (0.3 * unit(seed + across_at + (up_at * 17u))) : 1.0;
        cut.patch = near ? float2((uv.x + 0.5) * 5.0, uv.y * 7.0) : float2(0.0);
        // Limbs fanning up and out from low in the head, forking as they reach its edge; far
        // off, a thin haze of them.
        if (near) {
            const float2 from = float2(uv.x / 0.48, uv.y - (foot + (0.15 * (1.0 - foot))));
            const float fan = (atan2(from.x, max(from.y, 0.0) + 0.05) * 1.6) +
                              (0.06 * sin((length(from) * 9.0) + (6.2831853 * unit(seed + 12u))));
            const float forked = reach > 0.6 ? 2.0 : 1.0;
            cut.twig = abs(frac((fan * forked) + unit(seed + 13u)) - 0.5) < (0.09 - (0.04 * reach));
        } else {
            cut.twig = unit(seed + 21u) < 0.35;
        }
        cut.shade = lerp(0.6, 1.1, saturate((off.y * 0.5) + 0.5)) * mottle;
        return cut;
    }
    const float trunk = (0.035 * (1.0 - (0.5 * uv.y))) + 0.004;
    if (abs(uv.x) < trunk && uv.y < middle) {
        cut.part = bark;
    }
    return cut;
}

// What of the palm the point falls on: fronds springing from the top of a bare stem, arching out
// either way and down, each narrow at its stalk, combed into leaflets; coconuts under them once it
// bears. Never thinner than a pixel, so far off it stays a starburst rather than sparkling apart.
Cut palm_cut(uint seed, float age, float2 uv, float pixels) {
    Cut cut;
    cut.part = air_part;
    cut.shade = 1.0;
    cut.patch = float2(0.0);
    cut.twig = false;
    const float reach = palm_reach(seed, age);
    const float width = 2.2 * reach;
    // In tree heights from the crown's heart, which stands a frond's rise below the quad's top.
    const float top = 1.0 - (0.15 * reach);
    const float2 at = float2(uv.x * width, uv.y - top);
    const float least = 0.6 * width / max(pixels, 1.0);
    const float droop = across(age, 0.5, 1.25);
    for (uint frond = 0; frond < 4; ++frond) {
        // Two fronds either way in this quad's plane, the second turned partly toward the eye.
        const float way = ((frond & 1u) == 0 ? 1.0 : -1.0) * (frond < 2 ? 1.0 : 0.55);
        const float length = reach * (0.85 + (0.3 * unit(seed + 50u + frond)));
        const float t = at.x / (way * length);
        if (t < 0.0 || t > 1.0) {
            continue;
        }
        const float rise = lerp(0.1, 0.45, unit(seed + 30u + frond));
        const float fall = lerp(0.45, 0.75, unit(seed + 40u + frond)) * droop;
        const float off = at.y - (length * ((rise * t) - (fall * t * t)));
        const float slope = (rise - (2.0 * fall * t)) / abs(way);
        const float half_width =
            (length * 0.11 * (1.0 - (0.7 * t)) * saturate(t * 6.0)) + least;
        if (abs(off) < half_width * sqrt(1.0 + (slope * slope))) {
            cut.part = needles;
            // Leaflets combed along the frond, lit above its midrib and shaded below.
            const float comb = frac((t * 16.0) + unit(seed + 60u + frond));
            cut.shade = (off > 0.0 ? 1.05 : 0.8) * (0.8 + (0.3 * comb));
            cut.patch = float2(t * 5.0, float(frond));
            return cut;
        }
    }
    // The crown's heart, where the fronds spring from.
    if (length(at / float2(0.05 * reach, 0.035 * reach)) < 1.0) {
        cut.part = needles;
        cut.shade = 0.7;
        return cut;
    }
    if (pixels >= 7.0 && age > 0.35 && length((at - float2(0.0, -0.05 * reach)) /
                                              float2(0.07 * reach, 0.05 * reach)) < 1.0) {
        cut.part = bark;
        cut.shade = 0.8;
        return cut;
    }
    if (abs(at.x) < 0.012 + least && uv.y < top) {
        cut.part = bark;
    }
    return cut;
}

// Dice rolled at each whole point of `at` and blended smoothly between, 0 to 1.
float smooth_dice(uint seed, float2 at) {
    const float2 whole = floor(at);
    const float2 part = smoothstep(0.0, 1.0, at - whole);
    const uint i = uint(int(whole.x) + 64);
    const uint j = uint(int(whole.y) + 64);
    const float low = lerp(unit(seed + i + (j * 131u)), unit(seed + i + 1u + (j * 131u)), part.x);
    const float high =
        lerp(unit(seed + i + ((j + 1u) * 131u)), unit(seed + i + 1u + ((j + 1u) * 131u)), part.x);
    return lerp(low, high, part.y);
}

// Lit as the ground beneath it, as grass.slang lights its blades.
[shader("fragment")]
float4 fragment_main(Tree tree) : SV_Target {
    Cut cut;
    if (tree.genus == pine) {
        cut = pine_cut(tree.seed, tree.age, tree.uv, tree.pixels);
    } else if (tree.genus == palm) {
        cut = palm_cut(tree.seed, tree.age, tree.uv, tree.pixels);
    } else {
        cut = broadleaf_cut(tree.seed, tree.age, tree.uv, tree.pixels, head_of(tree.genus));
    }
    if (cut.part == air_part) {
        discard;
    }
    float3 albedo = woods[tree.genus] * (cut.part == bark ? cut.shade : 1.0);
    if (cut.part == needles) {
        // Each patch keeps the tree's dates give or take a week or so, the crown's top first;
        // where its leaves are down, the bare branches show through.
        const float own = tree.own + (0.6 * ((2.0 * smooth_dice(tree.seed + 1u, cut.patch)) - 1.0)) +
                          (0.3 * tree.uv.y);
        const Leaves leaves = leaves_at(settings.leafing[tree.genus], own, lighting.year);
        if (smooth_dice(tree.seed + 2u, cut.patch * 2.0) >= leaves.held) {
            if (!cut.twig) {
                discard;
            }
            albedo = woods[tree.genus] * 0.8;
        } else {
            albedo = autumn_colour(tree.colour, leaves.turned, settings.first[tree.genus].rgb,
                                   settings.last[tree.genus].rgb) *
                     tree.tone * cut.shade;
        }
    }
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
