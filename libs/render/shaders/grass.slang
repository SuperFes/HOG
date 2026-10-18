import atmosphere;
import bend;
import shadow;
import tone;

// Blades grown on the ground's own patches: each one hashed from its patch's seed, so every
// machine grows the same field, swaying with the weather's wind.

struct Push {
    float4x4 clip_from_eye;
    // xyz up through the eye, w its height above sea level.
    float4 bend_up;
    // x the planet's radius and y the faked one drawing bends it onto; z seconds of sway so far,
    // w the radians across a pixel.
    float4 bend_radii;
    // xyz the wind near the ground, m/s; w (as bits) the patch's seed.
    float4 wind;
    // xyz the patch's origin from the eye, in metres; w (as bits) the bytes before the patch's
    // vertices in what's bound, times 64, plus the blades grown in each grid cell.
    float4 origin;
};

[[vk::push_constant]] ConstantBuffer<Push> push;

static const uint biome_count = 14;

struct Settings {
    // Each biome's blades: rgb their colour, w their height in metres, none for no grass.
    float4 blade[biome_count];
    // x the share of blades grown, y their width in metres, z the share of clumps grown.
    float4 spread[biome_count];
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

// Metres from the eye blades keep their height to, and where they've shrunk away.
static const float fade_near = 25.0;
static const float fade_far = 55.0;
// Blades keep off ground this close to sea level, so the surf stays clear.
static const float shore = 0.15;

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

// The two triangles of a blade's quad: root left and right, then tip left and right.
static const uint corners[6] = { 0, 1, 2, 2, 1, 3 };

struct Blade {
    float4 position : SV_Position;
    float3 colour;
    float3 normal;
    float3 from_eye;
    float3 drawn; // from the eye, bent
    float height; // above sea level
};

// Nothing: a point's triangles cover no pixels.
Blade nothing() {
    Blade gone;
    gone.position = float4(0.0, 0.0, 0.0, 1.0);
    gone.colour = float3(0.0);
    gone.normal = float3(0.0, 0.0, 1.0);
    gone.from_eye = float3(0.0);
    gone.drawn = float3(0.0);
    gone.height = 0.0;
    return gone;
}

// How far the tip is held and swayed by the wind: held over in strong wind, swaying most in a
// breeze, still in still air.
float3 blown(float3 root, float height, uint hash) {
    const float speed = length(push.wind.xyz);
    if (speed < 1e-3) {
        return float3(0.0);
    }
    const float3 along = push.wind.xyz / speed;
    const float held = (speed * speed) / ((speed * speed) + 36.0);
    const float swaying = 0.3 * speed / (1.0 + (0.08 * speed * speed));
    const float time = push.bend_radii.z;
    // Waves cross the field along the wind, gusts rolling over them.
    const float phase = (time * (1.4 + (0.3 * speed))) - (dot(root, along) * 0.35) +
                        (6.2831853 * unit(hash));
    const float gust = 0.6 + (0.4 * sin((time * 0.7) - (dot(root, along) * 0.05)));
    return along * (height * (held + (swaying * gust * sin(phase))));
}

[shader("vertex")]
Blade vertex_main(uint vertex: SV_VertexID, uint instance: SV_InstanceID) {
    const uint packed = asuint(push.origin.w);
    const uint bias = packed >> 6;
    const uint blades = packed & 63u;
    const uint seed = asuint(push.wind.w);

    const uint cell = instance / blades;
    const uint i = cell >> 5;
    const uint j = cell & 31u;
    const uint hash = hashed(seed ^ instance);

    // Rooted within its grid cell, on ground interpolated between the cell's corners.
    const float u = unit(hash);
    const float v = unit(hash + 1u);
    const uint at = (i * patch_side) + j;
    const Corner near = corner_at(bias, at + (u < 0.5 ? 0 : patch_side) + (v < 0.5 ? 0 : 1));
    if (settings.blade[near.biome].w <= 0.0 ||
        unit(hash + 2u) >= settings.spread[near.biome].x) {
        return nothing();
    }
    // Sparse biomes grow in clumps rather than an evenly thinned lawn: desert bunchgrass stands
    // in tufts of its own, bare ground between.
    const uint clump = ((i >> 2) << 3) | (j >> 2);
    if (unit(hashed(seed ^ (0x517Cu + clump))) >= settings.spread[near.biome].z) {
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
    const float fade = saturate((fade_far - length(root)) / (fade_far - fade_near));
    if (above < shore || fade <= 0.0) {
        return nothing();
    }

    // Leaning its own slight way, facing its own way.
    const float3 beside = normalize(abs(normal.z) < 0.9 ? cross(normal, float3(0.0, 0.0, 1.0))
                                                        : cross(normal, float3(1.0, 0.0, 0.0)));
    const float3 ahead = cross(normal, beside);
    const float facing = 6.2831853 * unit(hash + 3u);
    const float3 width_dir = (beside * cos(facing)) + (ahead * sin(facing));
    const float leaning = 6.2831853 * unit(hash + 4u);
    const float3 up =
        normalize(normal + (0.2 * ((beside * cos(leaning)) + (ahead * sin(leaning)))));

    const float height =
        settings.blade[near.biome].w * (0.6 + (0.8 * unit(hash + 5u))) * fade;
    // At least a pixel wide, so far blades don't shatter into bright specks; drawn wider than
    // they are, they wear the ground's colour instead, melting into it.
    const float true_width = settings.spread[near.biome].y * (0.7 + (0.6 * unit(hash + 6u)));
    const float pixel = push.bend_radii.w * length(root);
    const float width = max(true_width, 0.8 * pixel);
    const float itself = true_width / width;
    const float3 tip_away = blown(root, height, hash + 7u);

    // Two triangles tapering to a narrow tip, the tip carried by the wind.
    const uint corner = corners[vertex];
    const float side = (corner & 1u) == 0 ? -0.5 : 0.5;
    const float t = corner < 2 ? 0.0 : 1.0;
    const float3 placed = root + (width_dir * (side * width * (1.0 - (0.8 * t)))) +
                          (up * (height * t)) + (tip_away * (t * t));

    Blade blade;
    blade.from_eye = placed;
    blade.drawn = bent(placed, push.bend_up, push.bend_radii.xy);
    blade.position = mul(push.clip_from_eye, float4(blade.drawn, 1.0));
    blade.normal = normal;
    blade.height = above;
    // Darker towards the root, as fields shade themselves; the ground's own tone where the blade
    // is really thinner than drawn.
    const float its_own = (0.8 + (0.4 * unit(hash + 8u))) * (0.55 + (0.45 * t));
    blade.colour = settings.blade[near.biome].rgb * lerp(1.25, its_own, itself);
    return blade;
}

// Lit as the ground beneath it: by sunlight through the air and the sky's light, seen through the
// air between, and tone mapped, as terrain.slang lights the ground.
[shader("fragment")]
float4 fragment_main(Blade blade) : SV_Target {
    const float3 sun = lighting.sun_exposure.xyz;
    const float scale = lighting.sky_scale.w;
    const float3 normal = normalize(blade.normal);
    const float3 up = normalize(blade.from_eye +
                                (push.bend_up.xyz * (push.bend_radii.x + push.bend_up.w)));
    const float3 clear =
        transmittance(air, transmittance_table, max(blade.height, 0.0) * scale, dot(up, sun));
    const Shade shade = shade_at(lighting, cloud_shadow, blade.drawn);
    const float3 sunlight = through_cloud(shade, clear);
    const float3 sky = sky_under_cloud(lighting, shade, clear * saturate(dot(up, sun)));
    const float3 lit =
        (sunlight * saturate(dot(normal, sun))) + (sky * (0.5 + (0.5 * dot(normal, up))));
    const float3 radiance = blade.colour * lit / pi;

    const float4 clip = mul(push.clip_from_eye, float4(blade.drawn, 1.0));
    const float2 uv = ((clip.xy / clip.w) * 0.5) + 0.5;
    const float4 between =
        aerial_perspective(aerial_volume, uv, length(blade.drawn) * scale, lighting.reach.x);
    return float4(tone_mapped((radiance * between.a) + between.rgb, lighting.sun_exposure.w), 1.0);
}
