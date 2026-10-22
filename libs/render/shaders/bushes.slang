import atmosphere;
import bend;
import flora;
import shadow;
import tone;
import water;

// Bushes drawn from their anchors, each bound to its spot in the world, so the same bush stands in
// the same spot at every terrain level and on every machine. One bush is three quads: crossed
// upright, or for the round kinds two crossed upright and a lid lying across them, so they're
// round from overhead too. A mask cut in the fragment shader shapes its silhouette by kind: a
// floofy blob, a spiky rosette, a twiggy skeleton, or a low mat.

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
    // x metres from the eye bushes have shrunk away by.
    float4 reach;
};

[[vk::binding(0, 0)]] ConstantBuffer<Settings> settings;
// The tile's bush anchors, as render::BushAnchor lies.
[[vk::binding(1, 0)]] ByteAddressBuffer anchors;
[[vk::binding(2, 0)]] ConstantBuffer<Air> air;
[[vk::binding(3, 0)]] ConstantBuffer<Lighting> lighting;
[[vk::binding(4, 0)]] Sampler2D transmittance_table;
[[vk::binding(5, 0)]] Sampler2DArray aerial_volume;
// As cloud_march.slang's.
[[vk::binding(6, 0)]] Sampler2D cloud_shadow;

static const uint anchor_stride = 40; // bytes, as render::BushAnchor lies

// Bare wood, for twiggy skeletons and the shade under leaves.
static const float3 wood = float3(0.09, 0.055, 0.03);

// As flora::BushKind.
static const uint floofy = 0;
static const uint spiky = 1;
static const uint twiggy = 2;
static const uint mat = 3;

// The two triangles of a quad: bottom left and right, then top left and right.
static const uint corners[6] = { 0, 1, 2, 2, 1, 3 };
// Of a round bush's height, where its lid lies, through its widest.
static const float lid_at = 0.5;
// Of a floofy bush's width, its lid's, within the outline its upright quads have there.
static const float floofy_lid = 0.9;

struct Bush {
    float4 position : SV_Position;
    float3 colour;
    float3 normal;
    float3 from_eye;
    float3 drawn; // from the eye, bent
    float height; // above sea level
    float2 uv;    // -0.5 to 0.5 across the quad, 0 to 1 up it
    float pixels; // the quad's width in pixels, for shedding detail far off
    nointerpolation uint kind;
    nointerpolation uint seed; // the bush's own, keyed to its lattice cell
    nointerpolation uint lying; // 1 on the lid
};

// Nothing: a point's triangles cover no pixels.
Bush nothing() {
    Bush gone;
    gone.position = float4(0.0, 0.0, 0.0, 1.0);
    gone.colour = float3(0.0);
    gone.normal = float3(0.0, 0.0, 1.0);
    gone.from_eye = float3(0.0);
    gone.drawn = float3(0.0);
    gone.height = 0.0;
    gone.uv = float2(0.0);
    gone.pixels = 0.0;
    gone.kind = floofy;
    gone.seed = 0;
    gone.lying = 0;
    return gone;
}

[shader("vertex")]
Bush vertex_main(uint vertex: SV_VertexID, uint instance: SV_InstanceID) {
    const uint at = (asuint(push.origin.w) + instance) * anchor_stride;
    const float3 root = push.origin.xyz + asfloat(anchors.Load3(at));
    const float3 normal = asfloat(anchors.Load3(at + 16));
    const uint hash = anchors.Load(at + 28);
    const uint packed = anchors.Load(at + 32);
    const uint kind = (packed >> 24) & 3u;
    const float3 rooted = float3(float(packed & 0xffu), float((packed >> 8) & 0xffu),
                                 float((packed >> 16) & 0xffu)) / 255.0;
    const float3 leaf = rooted * rooted;

    const float away = length(root);
    const float fade_far = settings.reach.x;
    // Full sized until near the edge of their reach, then shrinking away.
    const float fade = saturate((fade_far - away) / (0.2 * fade_far));
    if (fade <= 0.0) {
        return nothing();
    }
    const float height = asfloat(anchors.Load(at + 12)) * fade;
    const float width = asfloat(anchors.Load(at + 36)) * fade;

    // Quads crossed about the bush's own up, the mask doing the shaping; a round bush's third
    // lies across the other two.
    const uint quad = vertex / 6;
    const uint corner = corners[vertex % 6];
    const bool round = kind == floofy || kind == mat;
    const bool lying = round && quad == 2;
    const float3 beside = normalize(abs(normal.z) < 0.9 ? cross(normal, float3(0.0, 0.0, 1.0))
                                                        : cross(normal, float3(1.0, 0.0, 0.0)));
    const float3 ahead = cross(normal, beside);
    const float apart = round ? 1.5707963 : 1.0471976;
    const float facing = (float(lying ? 0 : quad) * apart) + (6.2831853 * unit(hash + 5u));
    const float3 width_dir = (beside * cos(facing)) + (ahead * sin(facing));
    const float3 across = cross(normal, width_dir);

    const float side_x = (corner & 1u) == 0 ? -0.5 : 0.5;
    const float t = corner < 2 ? 0.0 : 1.0;
    const float up_by = lying ? lid_at : t;
    // A lean held by strong wind and a slight rustle, stiffer than grass by far.
    const float speed = length(push.wind.xyz);
    float3 sway = float3(0.0);
    if (speed > 1e-3) {
        const float3 along = push.wind.xyz / speed;
        const float held = (speed * speed) / ((speed * speed) + 220.0);
        const float rustle =
            0.04 * sin((push.bend_radii.z * (1.1 + (0.2 * speed))) + (6.2831853 * unit(hash + 6u)));
        sway = along * (height * (held * 0.25 + rustle * saturate(speed / 6.0)));
    }
    const float lid = kind == floofy ? floofy_lid : 1.0;
    const float3 spread =
        lying ? (((width_dir * side_x) + (across * (t - 0.5))) * (width * lid)) +
                    (normal * (height * lid_at))
              : (width_dir * (side_x * width)) + (normal * (height * t));
    const float3 placed = root + spread + (sway * (up_by * up_by));

    Bush bush;
    bush.from_eye = placed;
    bush.drawn = bent(placed, push.bend_up, push.bend_radii.xy);
    bush.position = mul(push.clip_from_eye, float4(bush.drawn, 1.0));
    // Rounded off per corner, so the flat quads shade like a volume.
    bush.normal =
        lying ? normalize(normal + (((width_dir * side_x) + (across * (t - 0.5))) * 1.2))
              : normalize(normal + (width_dir * (side_x * 1.2)) + (normal * ((t - 0.4) * 0.6)));
    bush.height = height_above_sea(root, push.bend_up, push.bend_radii.x);
    bush.pixels = width / max(push.bend_radii.w * away, 1e-6);
    // Bare wood greys toward the leaves' tone far off, where twigs blur into haze.
    const float hazed = saturate((12.0 - bush.pixels) / 8.0);
    const float3 own = kind == twiggy ? lerp(wood, leaf * 0.8, hazed) : leaf;
    // The lid is the top, seen from overhead.
    bush.colour = own * (0.8 + (0.4 * unit(hash + 7u))) * (0.6 + (0.4 * (lying ? 1.0 : t)));
    bush.uv = float2(side_x, t);
    bush.kind = kind;
    bush.seed = hash;
    bush.lying = lying ? 1u : 0u;
    return bush;
}

// Whether the point is within the round bush's outline seen from overhead, on its lid.
bool within_lid(uint kind, uint seed, float2 uv, float pixels) {
    const float2 off = uv - float2(0.0, 0.5);
    if (pixels < 8.0) {
        return dot(off, off) < 0.45 * 0.45;
    }
    if (kind == floofy) {
        for (uint k = 0; k < 5; ++k) {
            const float2 centre = (float2(unit(seed + 11u + k), unit(seed + 17u + k)) - 0.5) * 0.5;
            const float radius = 0.2 + (0.12 * unit(seed + 23u + k));
            const float2 from = off - centre;
            if (dot(from, from) < radius * radius) {
                return true;
            }
        }
        return false;
    }
    const float angle = atan2(off.x, off.y);
    const float scallop = 1.0 + (0.12 * sin((angle * 7.0) + (6.2831853 * unit(seed + 47u))));
    return dot(off, off) < 0.47 * 0.47 * scallop;
}

// Whether the point is within the bush's silhouette; shapes shed their detail once the whole
// quad is only a few pixels across.
bool within(uint kind, uint seed, float2 uv, float pixels) {
    if (pixels < 8.0) {
        // Far off, a plain tuft: an ellipse over the quad's middle. A twiggy skeleton is mostly
        // air, so far off it is only a wisp.
        const float2 radii = kind == twiggy ? float2(0.2, 0.22) : float2(0.45, 0.48);
        const float2 off = (uv - float2(0.0, 0.5)) / radii;
        return dot(off, off) < 1.0;
    }
    if (kind == floofy) {
        // A cluster of discs, scalloped like the clouds.
        for (uint k = 0; k < 5; ++k) {
            const float2 centre = float2((unit(seed + 11u + k) - 0.5) * 0.5,
                                         0.45 + ((unit(seed + 17u + k) - 0.5) * 0.45));
            const float radius = 0.2 + (0.12 * unit(seed + 23u + k));
            const float2 off = uv - centre;
            if (dot(off, off) < radius * radius) {
                return true;
            }
        }
        return false;
    }
    if (kind == spiky) {
        // Pointed blades fanned from the base, a rosette.
        const float2 off = uv - float2(0.0, 0.05);
        if (off.y < 0.0) {
            return false;
        }
        const float angle = atan2(off.x, off.y); // 0 straight up
        const float teeth = frac(((angle / 3.1415927) + 1.0) * 5.0 + unit(seed + 31u));
        const float tooth = 1.0 - (2.0 * abs(teeth - 0.5));
        const float reach = 0.95 * (0.3 + (0.7 * tooth * tooth * tooth));
        return length(off) < reach && abs(angle) < 1.45;
    }
    if (kind == twiggy) {
        // A few bare forking branches; everything else is air.
        for (uint k = 0; k < 6; ++k) {
            const uint parent = k % 3;
            const float swing = (unit(seed + 37u + parent) - 0.5) * 2.2;
            const float2 dir = normalize(float2(sin(swing), cos(swing)));
            float2 from = float2(0.0, 0.0);
            float2 heading = dir;
            float reach = 0.95;
            if (k >= 3) {
                // A fork from its parent's middle, swung aside.
                from = dir * 0.45;
                const float fork = swing + ((unit(seed + 43u + k) - 0.5) * 1.6);
                heading = normalize(float2(sin(fork), cos(fork)));
                reach = 0.45;
            }
            const float along = clamp(dot(uv - from, heading), 0.0, reach);
            const float2 off = uv - (from + (heading * along));
            const float thick = 0.028 * (1.0 - (along / reach)) + 0.006;
            if (dot(off, off) < thick * thick) {
                return true;
            }
        }
        return false;
    }
    // A low mat: one squashed, scalloped cushion.
    const float2 off = (uv - float2(0.0, 0.4)) / float2(0.47, 0.42);
    const float angle = atan2(off.x, off.y);
    const float scallop = 1.0 + (0.12 * sin((angle * 7.0) + (6.2831853 * unit(seed + 47u))));
    return dot(off, off) < scallop;
}

// Lit as the ground beneath it, as grass.slang lights its blades.
[shader("fragment")]
float4 fragment_main(Bush bush) : SV_Target {
    const bool shaped = bush.lying != 0 ? within_lid(bush.kind, bush.seed, bush.uv, bush.pixels)
                                        : within(bush.kind, bush.seed, bush.uv, bush.pixels);
    if (!shaped) {
        discard;
    }
    float3 albedo = bush.colour;
    // The odd floofy bush flowers: a few bright dots, seen up close.
    if (bush.kind == floofy && bush.pixels > 24.0 && unit(bush.seed + 53u) < 0.3) {
        const float3 bloom = unit(bush.seed + 54u) < 0.5 ? float3(0.45, 0.08, 0.1)
                                                         : float3(0.5, 0.4, 0.05);
        for (uint k = 0; k < 3; ++k) {
            const float2 dot_at = float2((unit(bush.seed + 55u + k) - 0.5) * 0.6,
                                         0.35 + (0.5 * unit(bush.seed + 61u + k)));
            if (length(bush.uv - dot_at) < 0.035) {
                albedo = bloom;
            }
        }
    }
    const float3 sun = lighting.sun_exposure.xyz;
    const float scale = lighting.sky_scale.w;
    const float3 normal = normalize(bush.normal);
    const float3 up = normalize(bush.from_eye +
                                (push.bend_up.xyz * (push.bend_radii.x + push.bend_up.w)));
    const float3 clear =
        transmittance(air, transmittance_table, max(bush.height, 0.0) * scale, dot(up, sun));
    const Shade shade = shade_at(lighting, cloud_shadow, bush.drawn);
    const float3 sunlight = through_cloud(shade, clear);
    const float3 sky = sky_under_cloud(lighting, shade, clear * saturate(dot(up, sun)));
    const float3 lit =
        (sunlight * saturate(dot(normal, sun))) + (sky * (0.5 + (0.5 * dot(normal, up))));
    const float3 radiance =
        through_water_at(albedo * lit / pi, ((sunlight * saturate(dot(up, sun))) + sky) / pi,
                         bush.from_eye, push.bend_up, push.bend_radii.x,
                         sea_lays_water(lighting));

    const float4 clip = mul(push.clip_from_eye, float4(bush.drawn, 1.0));
    const float2 uv = ((clip.xy / clip.w) * 0.5) + 0.5;
    const float4 between =
        aerial_perspective(aerial_volume, uv, length(bush.drawn) * scale, lighting.reach.x);
    return float4(tone_mapped((radiance * between.a) + between.rgb, lighting.sun_exposure.w), 1.0);
}
