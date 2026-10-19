import atmosphere;
import bend;
import shadow;
import tone;

// The grown forest: whole pines from libs/flora, a few variants baked at a few details, each
// planted on a tree anchor bound to its spot in the world. The far stamps draw from the very same
// anchors, so the handoff at the forest's reach swaps representation, never trees.

struct Push {
    float4x4 clip_from_eye;
    // xyz up through the eye, w its height above sea level.
    float4 bend_up;
    // x the planet's radius and y the faked one drawing bends it onto; z seconds of sway so far,
    // w the radians across a pixel.
    float4 bend_radii;
    // xyz the wind near the ground, m/s; w unused.
    float4 wind;
    // xyz the tile's origin from the eye, in metres; w as bits: the low two the finest detail the
    // draw's nearest tree needs, bounding its vertex count, the rest the tile's first anchor drawn.
    float4 origin;
};

[[vk::push_constant]] ConstantBuffer<Push> push;

static const uint detail_count = 3;
static const uint stage_count = 4;
static const uint variant_count = 8;

// Where each variant's wood and leaves lie in the carpentry, per detail and stage of growth: x the
// vertex bytes' start, y the index bytes' start, z the index count; wood's w the baked height's
// bits.
struct Build {
    uint4 wood;
    uint4 leaf;
};

struct Settings {
    // Metres from the eye: x the fullest detail's reach, y the middle one's, z the forest's own,
    // where the stamps take over.
    float4 bands;
    // The ages between one baked stage and the next, sapling to old; w unused.
    float4 stages;
    Build builds[detail_count * stage_count * variant_count];
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
// Every variant's baked meshes at every detail, as render::Forest bakes them.
[[vk::binding(7, 0)]] ByteAddressBuffer carpentry;

static const uint anchor_stride = 40; // bytes, as render::TreeAnchor lies
static const uint wood_stride = 24;   // bytes, as flora::WoodVertex lies
static const uint leaf_stride = 36;   // bytes, as flora::LeafVertex lies

static const float3 bark = float3(0.055, 0.045, 0.038);

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

// As render::needles_of unpacks them: square-rooted bytes, squared back.
float3 needles_of(uint packed) {
    const float3 rooted = float3(float(packed & 0xffu), float((packed >> 8) & 0xffu),
                                 float((packed >> 16) & 0xffu)) / 255.0;
    return rooted * rooted;
}

struct Planted {
    bool grows;
    float3 root;      // from the eye
    float3 upright;   // the tree's own up
    float3 beside;    // the tree's local x, yawed its own way
    float3 ahead;     // its local y
    float scale;      // over the baked tree
    float sway;       // metres the crown's top is held over
    float3 along;     // the wind's direction
    float height_above; // of its ground, over the sea
    uint hash;
    uint build;       // into the carpentry's builds
};

// Where the draw's instance lies among the tile's anchors.
uint anchored(uint instance) {
    return ((asuint(push.origin.w) >> 2) + instance) * anchor_stride;
}

// The anchored tree, stood on its own spot: the anchor decided where, how tall, how old and
// whether long ago; here it only steps aside for the stamps beyond the forest's reach.
Planted plant(uint instance) {
    Planted planted;
    planted.grows = false;
    const uint at = anchored(instance);
    const float3 root = push.origin.xyz + asfloat(anchors.Load3(at));
    const float grown = asfloat(anchors.Load(at + 12));
    const float3 normal = asfloat(anchors.Load3(at + 16));
    const uint hash = anchors.Load(at + 28);
    const float age = asfloat(anchors.Load(at + 36));

    const float away = length(root);
    if (away >= settings.bands.z) {
        return planted;
    }
    // Each tree sheds detail by its own distance, never finer than the draw's vertex budget.
    const uint finest = asuint(push.origin.w) & 3u;
    uint lod = away < settings.bands.x ? 0u : (away < settings.bands.y ? 1u : 2u);
    lod = max(lod, finest);

    // A real tree grows for the light, not square to its slope — near vertical, with a shade of
    // the ground's lean. The stamps stand along the very same axis.
    const float3 upright = normalize(normal + (push.bend_up.xyz * 4.0));
    const float3 beside0 = normalize(abs(upright.z) < 0.9 ? cross(upright, float3(0.0, 0.0, 1.0))
                                                          : cross(upright, float3(1.0, 0.0, 0.0)));
    const float3 ahead0 = cross(upright, beside0);
    const float yaw = 6.2831853 * unit(hash + 3u);
    const float swing = cos(yaw);
    const float turn = sin(yaw);
    planted.beside = (beside0 * swing) + (ahead0 * turn);
    planted.ahead = (ahead0 * swing) - (beside0 * turn);
    planted.upright = upright;

    const uint variant = hashed(hash + 9u) % variant_count;
    const uint stage = uint(age >= settings.stages.x) + uint(age >= settings.stages.y) +
                       uint(age >= settings.stages.z);
    planted.build = (((lod * stage_count) + stage) * variant_count) + variant;
    const Build build = settings.builds[planted.build];
    planted.scale = grown / max(asfloat(build.wood.w), 0.1);

    const float speed = length(push.wind.xyz);
    planted.sway = 0.0;
    planted.along = float3(0.0);
    if (speed > 1e-3) {
        planted.along = push.wind.xyz / speed;
        const float held = (speed * speed) / ((speed * speed) + 320.0);
        const float rock = 0.025 * sin((push.bend_radii.z * (0.9 + (0.15 * speed))) +
                                       (6.2831853 * unit(hash + 6u)));
        planted.sway = grown * ((held * 0.16) + (rock * saturate(speed / 8.0)));
    }
    planted.grows = true;
    planted.root = root;
    planted.height_above = height_above_sea(root, push.bend_up, push.bend_radii.x);
    planted.hash = hash;
    return planted;
}

// A baked local point carried to where its tree stands, the crown held over by the wind.
float3 carried(const Planted planted, float3 local, float height_baked) {
    const float3 scaled = local * planted.scale;
    float3 world = planted.root + (planted.beside * scaled.x) + (planted.ahead * scaled.y) +
                   (planted.upright * scaled.z);
    const float lever = scaled.z / max(height_baked * planted.scale, 0.1);
    return world + (planted.along * (planted.sway * lever * lever));
}

float3 turned(const Planted planted, float3 local) {
    return normalize((planted.beside * local.x) + (planted.ahead * local.y) +
                     (planted.upright * local.z));
}

struct Timber {
    float4 position : SV_Position;
    float3 colour;
    float3 normal;
    float3 from_eye;
    float3 drawn; // from the eye, bent
    float height; // above sea level
};

Timber no_timber() {
    Timber gone;
    gone.position = float4(0.0, 0.0, 0.0, 1.0);
    gone.colour = float3(0.0);
    gone.normal = float3(0.0, 0.0, 1.0);
    gone.from_eye = float3(0.0);
    gone.drawn = float3(0.0);
    gone.height = 0.0;
    return gone;
}

[shader("vertex")]
Timber wood_vertex(uint vertex: SV_VertexID, uint instance: SV_InstanceID) {
    const Planted planted = plant(instance);
    if (!planted.grows) {
        return no_timber();
    }
    const Build build = settings.builds[planted.build];
    if (vertex >= build.wood.z) {
        return no_timber();
    }
    const uint index = carpentry.Load(build.wood.y + (vertex * 4));
    const uint at = build.wood.x + (index * wood_stride);
    const float3 local = asfloat(carpentry.Load3(at));
    const float3 local_normal = asfloat(carpentry.Load3(at + 12));

    Timber timber;
    timber.from_eye = carried(planted, local, asfloat(build.wood.w));
    timber.drawn = bent(timber.from_eye, push.bend_up, push.bend_radii.xy);
    timber.position = mul(push.clip_from_eye, float4(timber.drawn, 1.0));
    timber.normal = turned(planted, local_normal);
    timber.colour = bark * (0.85 + (0.3 * unit(planted.hash + 7u)));
    timber.height = planted.height_above;
    return timber;
}

// Lit as the ground beneath it, as the grass and stamps are; needles take the sun wrapped a
// little around them, as light combs through a crown.
float3 forest_lit(float3 albedo, float3 normal_raw, float3 from_eye, float3 drawn, float above,
                  float wrap) {
    const float3 sun = lighting.sun_exposure.xyz;
    const float scale = lighting.sky_scale.w;
    const float3 normal = normalize(normal_raw);
    const float3 up =
        normalize(from_eye + (push.bend_up.xyz * (push.bend_radii.x + push.bend_up.w)));
    const float3 clear =
        transmittance(air, transmittance_table, max(above, 0.0) * scale, dot(up, sun));
    const Shade shade = shade_at(lighting, cloud_shadow, drawn);
    const float3 sunlight = through_cloud(shade, clear);
    const float3 sky = sky_under_cloud(lighting, shade, clear * saturate(dot(up, sun)));
    const float3 lit =
        (sunlight * saturate((dot(normal, sun) + wrap) / (1.0 + wrap))) +
        (sky * (0.5 + (0.5 * dot(normal, up))));
    const float3 radiance = albedo * lit / pi;
    const float4 clip = mul(push.clip_from_eye, float4(drawn, 1.0));
    const float2 uv = ((clip.xy / clip.w) * 0.5) + 0.5;
    const float4 between =
        aerial_perspective(aerial_volume, uv, length(drawn) * scale, lighting.reach.x);
    return tone_mapped((radiance * between.a) + between.rgb, lighting.sun_exposure.w);
}

[shader("fragment")]
float4 wood_fragment(Timber timber) : SV_Target {
    return float4(forest_lit(timber.colour, timber.normal, timber.from_eye, timber.drawn,
                             timber.height, 0.0),
                  1.0);
}

struct Spray {
    float4 position : SV_Position;
    float3 colour;
    float3 normal;
    float3 from_eye;
    float3 drawn;
    float height;
    float2 card;  // -0.5 to 0.5 across the frond, 0 at its spine; 0 to 1 along it
    float pixels; // the frond's length in pixels, for shedding detail far off
    nointerpolation uint seed;
};

Spray no_spray() {
    Spray gone;
    gone.position = float4(0.0, 0.0, 0.0, 1.0);
    gone.colour = float3(0.0);
    gone.normal = float3(0.0, 0.0, 1.0);
    gone.from_eye = float3(0.0);
    gone.drawn = float3(0.0);
    gone.height = 0.0;
    gone.card = float2(0.0);
    gone.pixels = 0.0;
    gone.seed = 0;
    return gone;
}

[shader("vertex")]
Spray leaf_vertex(uint vertex: SV_VertexID, uint instance: SV_InstanceID) {
    const Planted planted = plant(instance);
    if (!planted.grows) {
        return no_spray();
    }
    const Build build = settings.builds[planted.build];
    if (vertex >= build.leaf.z) {
        return no_spray();
    }
    const float3 needles = needles_of(anchors.Load(anchored(instance) + 32));
    const uint index = carpentry.Load(build.leaf.y + (vertex * 4));
    const uint at = build.leaf.x + (index * leaf_stride);
    const float3 local = asfloat(carpentry.Load3(at));
    const float3 local_normal = asfloat(carpentry.Load3(at + 12));
    const float2 card = asfloat(carpentry.Load2(at + 24));
    const uint seed = carpentry.Load(at + 32);

    Spray spray;
    spray.from_eye = carried(planted, local, asfloat(build.wood.w));
    spray.drawn = bent(spray.from_eye, push.bend_up, push.bend_radii.xy);
    spray.position = mul(push.clip_from_eye, float4(spray.drawn, 1.0));
    // Rounded out from the trunk, so the fronds shade like a crown's volume as well as their own.
    const float3 bulge = float3(local.x, local.y, 0.35);
    spray.normal = turned(planted, normalize((local_normal * 0.8) +
                                             (normalize(bulge) * 0.5)));
    // Toned as the stamps tone the same tree: the tree's own cast, darker low in the crown.
    const float toward_top = saturate(local.z / max(asfloat(build.wood.w), 0.1));
    spray.colour = needles * (0.75 + (0.45 * unit(planted.hash + 7u))) *
                   (0.85 + (0.3 * unit(seed + 1u))) * (0.75 + (0.25 * toward_top));
    spray.height = planted.height_above;
    spray.card = card;
    spray.pixels =
        (2.4 * planted.scale) / max(push.bend_radii.w * max(length(planted.root), 1.0), 1e-6);
    spray.seed = seed;
    return spray;
}

// The needles' own texture: streaks combing from the spine outward and toward the tip, each a
// shade its own, melting to one tone once the frond is only a few pixels long.
float needled(uint seed, float2 card, float pixels) {
    if (pixels < 12.0) {
        return 1.0;
    }
    const float comb = (abs(card.x) * 18.0) + ((1.0 - card.y) * 11.0);
    const float streak = unit(seed + uint(comb));
    const float melt = saturate((pixels - 12.0) / 24.0);
    return 1.0 + (melt * 0.5 * (streak - 0.5));
}

[shader("fragment")]
float4 leaf_fragment(Spray spray, bool front: SV_IsFrontFace) : SV_Target {
    // Brightest along the outer needles, shadowed at the stem, dark beneath the bough.
    const float shaped = (0.75 + (0.5 * saturate(abs(spray.card.x) * 2.0))) * (front ? 1.0 : 0.72);
    const float3 albedo = spray.colour * shaped * needled(spray.seed, spray.card, spray.pixels);
    return float4(
        forest_lit(albedo, spray.normal, spray.from_eye, spray.drawn, spray.height, 0.3), 1.0);
}
