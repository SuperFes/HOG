import atmosphere;
import bend;
import flora;
import shadow;
import tone;

// Bushes grown on the ground's own patches, each keyed to a fixed lattice of level-16 cells so
// the same bush stands in the same spot at every terrain level and on every machine. One bush is
// three crossed quads; a mask cut in the fragment shader shapes its silhouette by kind: a floofy
// blob, a spiky rosette, a twiggy skeleton, or a low mat.

struct Push {
    float4x4 clip_from_eye;
    // xyz up through the eye, w its height above sea level.
    float4 bend_up;
    // x the planet's radius and y the faked one drawing bends it onto; z seconds of sway so far,
    // w the radians across a pixel.
    float4 bend_radii;
    // xyz the wind near the ground, m/s; w (as bits) the patch's first lattice cell, i then j,
    // sixteen bits each.
    float4 wind;
    // xyz the patch's origin from the eye, in metres; w (as bits) the bytes before the patch's
    // vertices in what's bound, the face in three bits, the lattice shift plus one in three, and
    // the metres bushes have shrunk away by in twelve.
    float4 origin;
};

[[vk::push_constant]] ConstantBuffer<Push> push;

static const uint biome_count = 14;

struct Settings {
    // Each biome's bushes: rgb their leaves' colour, w their height in metres, none grows none.
    float4 leaf[biome_count];
    // x the share of lattice cells grown, then the kinds' shares: y floofy blobs, z spiky
    // rosettes, w twiggy skeletons; low mats take the rest.
    float4 shapes[biome_count];
    // The biome's tree stands: x the stand field's woodland threshold, y whether any woods grow;
    // zw reserved. Bushes crowd the woods' edges and thin under the canopy.
    float4 woods[biome_count];
};

[[vk::binding(0, 0)]] ConstantBuffer<Settings> settings;
// The pool's patch vertices, as render::TerrainPool lays them out.
[[vk::binding(1, 0)]] ByteAddressBuffer vertices;
[[vk::binding(2, 0)]] ConstantBuffer<Air> air;
[[vk::binding(3, 0)]] ConstantBuffer<Lighting> lighting;
[[vk::binding(4, 0)]] Sampler2D transmittance_table;
[[vk::binding(5, 0)]] Sampler2DArray aerial_volume;
// As cloud_march.slang's.
[[vk::binding(6, 0)]] Sampler2D cloud_shadow;

static const uint patch_cells = 32;
static const uint patch_side = 33;
static const uint vertex_stride = 28; // bytes, as terrain::PatchVertex lies

// Bushes keep off ground this close to sea level.
static const float shore = 0.3;
// Bare wood, for twiggy skeletons and the shade under leaves.
static const float3 wood = float3(0.09, 0.055, 0.03);

static const uint floofy = 0;
static const uint spiky = 1;
static const uint twiggy = 2;
static const uint mat = 3;

struct Corner {
    float3 position;
    float3 normal;
    uint biome;
};

Corner corner_at(uint bias, uint index) {
    const uint at = bias + (index * vertex_stride);
    Corner corner;
    corner.position = asfloat(vertices.Load3(at));
    corner.normal = asfloat(vertices.Load3(at + 12));
    corner.biome = min(vertices.Load(at + 24) & 0xffu, biome_count - 1);
    return corner;
}

// The two triangles of a quad: bottom left and right, then top left and right.
static const uint corners[6] = { 0, 1, 2, 2, 1, 3 };

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
    return gone;
}

[shader("vertex")]
Bush vertex_main(uint vertex: SV_VertexID, uint instance: SV_InstanceID) {
    const uint packed = asuint(push.origin.w);
    const uint bias = packed & 0xffu;
    const uint face = (packed >> 8) & 7u;
    const int shift = int((packed >> 11) & 7u) - 1;
    const float fade_far = float((packed >> 14) & 0xfffu);
    const uint base = asuint(push.wind.w);
    const uint lattice_i = base >> 16;
    const uint lattice_j = base & 0xffffu;

    // The bush's lattice cell, local then global, keyed the same from every patch over it.
    const uint side = shift >= 0 ? (patch_cells >> shift) : (patch_cells << uint(-shift));
    const uint li = instance / side;
    const uint lj = instance % side;
    const uint hash =
        hashed(hashed((lattice_i + li) ^ (face * 0x9e3779b9u)) + (lattice_j + lj));

    // Rooted at its own spot within the lattice cell, in this patch's grid.
    const float across = shift >= 0 ? float(1u << uint(shift)) : 0.5;
    const float gi = (float(li) + unit(hash)) * across;
    const float gj = (float(lj) + unit(hash + 1u)) * across;
    const uint i = min(uint(gi), patch_cells - 1);
    const uint j = min(uint(gj), patch_cells - 1);
    const float u = gi - float(i);
    const float v = gj - float(j);

    const uint at = (i * patch_side) + j;
    const Corner near = corner_at(bias, at + (u < 0.5 ? 0 : patch_side) + (v < 0.5 ? 0 : 1));
    const float4 leaf = settings.leaf[near.biome];
    const float4 shapes = settings.shapes[near.biome];
    // Where trees stand, bushes keep to their place: crowding the woods' sunny edges, thinning
    // under the canopy, and spread as ever where no woods grow at all.
    const float4 woods = settings.woods[near.biome];
    float share = shapes.x;
    if (woods.y > 0.5) {
        // This lattice lies two levels finer than the trees'.
        const float2 spot = float2(float(lattice_i + li), float(lattice_j + lj)) * 0.25;
        const float field = stand_field(spot, face);
        const float canopy = smoothstep(woods.x, woods.x + 0.1, field);
        const float hem = saturate(1.0 - (abs(field - woods.x - 0.05) / 0.1));
        share *= lerp(1.0, 0.4, canopy) + (0.8 * hem);
    }
    if (leaf.w <= 0.0 || unit(hash + 2u) >= share) {
        return nothing();
    }
    const Corner c00 = corner_at(bias, at);
    const Corner c10 = corner_at(bias, at + patch_side);
    const Corner c01 = corner_at(bias, at + 1);
    const Corner c11 = corner_at(bias, at + patch_side + 1);
    const float3 ground = lerp(lerp(c00.position, c10.position, u),
                               lerp(c01.position, c11.position, u), v);
    const float3 normal =
        normalize(lerp(lerp(c00.normal, c10.normal, u), lerp(c01.normal, c11.normal, u), v));

    const float3 root = push.origin.xyz + ground;
    const float above = height_above_sea(root, push.bend_up, push.bend_radii.x);
    const float away = length(root);
    // Full sized until near the edge of their reach, then shrinking away.
    const float fade = saturate((fade_far - away) / (0.2 * fade_far));
    if (above < shore || fade <= 0.0) {
        return nothing();
    }

    // Its kind, rolled against the biome's mix.
    const float roll = unit(hash + 3u);
    uint kind = mat;
    if (roll < shapes.y) {
        kind = floofy;
    } else if (roll < shapes.y + shapes.z) {
        kind = spiky;
    } else if (roll < shapes.y + shapes.z + shapes.w) {
        kind = twiggy;
    }

    float height = leaf.w * (0.7 + (0.6 * unit(hash + 4u))) * fade;
    float width = height * 1.4;
    if (kind == spiky) {
        width = height;
    } else if (kind == twiggy) {
        width = height * 1.2;
    } else if (kind == mat) {
        height *= 0.4;
        width = height * 2.8;
    }

    // Three quads crossed about the bush's own up, the mask doing the shaping.
    const uint quad = vertex / 6;
    const uint corner = corners[vertex % 6];
    const float3 beside = normalize(abs(normal.z) < 0.9 ? cross(normal, float3(0.0, 0.0, 1.0))
                                                        : cross(normal, float3(1.0, 0.0, 0.0)));
    const float3 ahead = cross(normal, beside);
    const float facing = (float(quad) * 1.0471976) + (6.2831853 * unit(hash + 5u));
    const float3 width_dir = (beside * cos(facing)) + (ahead * sin(facing));

    const float side_x = (corner & 1u) == 0 ? -0.5 : 0.5;
    const float t = corner < 2 ? 0.0 : 1.0;
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
    const float3 placed =
        root + (width_dir * (side_x * width)) + (normal * (height * t)) + (sway * (t * t));

    Bush bush;
    bush.from_eye = placed;
    bush.drawn = bent(placed, push.bend_up, push.bend_radii.xy);
    bush.position = mul(push.clip_from_eye, float4(bush.drawn, 1.0));
    // Rounded off per corner, so the flat quads shade like a volume.
    bush.normal = normalize(normal + (width_dir * (side_x * 1.2)) + (normal * ((t - 0.4) * 0.6)));
    bush.height = above;
    bush.pixels = width / max(push.bend_radii.w * away, 1e-6);
    // Bare wood greys toward the leaves' tone far off, where twigs blur into haze.
    const float hazed = saturate((12.0 - bush.pixels) / 8.0);
    const float3 own = kind == twiggy ? lerp(wood, leaf.rgb * 0.8, hazed) : leaf.rgb;
    bush.colour = own * (0.8 + (0.4 * unit(hash + 7u))) * (0.6 + (0.4 * t));
    bush.uv = float2(side_x, t);
    bush.kind = kind;
    bush.seed = hash;
    return bush;
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
    if (!within(bush.kind, bush.seed, bush.uv, bush.pixels)) {
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
    const float3 radiance = albedo * lit / pi;

    const float4 clip = mul(push.clip_from_eye, float4(bush.drawn, 1.0));
    const float2 uv = ((clip.xy / clip.w) * 0.5) + 0.5;
    const float4 between =
        aerial_perspective(aerial_volume, uv, length(bush.drawn) * scale, lighting.reach.x);
    return float4(tone_mapped((radiance * between.a) + between.rgb, lighting.sun_exposure.w), 1.0);
}
