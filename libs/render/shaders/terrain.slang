import atmosphere;
import bend;
import projection;
import season;
import shadow;
import tone;
import water;
import waves;

struct Push {
    float4x4 clip_from_eye;
    // xyz towards the sun, w how bright ground facing away from it stays.
    float4 sun;
    // xyz up through the eye, w its height above sea level; then the planet's radius and the
    // faked one drawing bends it onto, and x and y of where the eye lies in the materials' tiling.
    float4 bend_up;
    float4 bend_radii;
    // xyz the patch's origin from the eye, in metres; w z of where the eye lies in the tiling.
    float4 origin;
};

[[vk::push_constant]] ConstantBuffer<Push> push;

static const uint biome_count = 14;
static const uint genus_count = 4;

struct Settings {
    // Metres a material spans near and far, and 1 if ground below sea level is seen through water.
    float4 tiling;
    // What each biome's material averages to.
    float4 means[biome_count];
    // x metres from the eye the tree stamps have shrunk away by, 0 for no woods drawn in the
    // ground.
    float4 canopy;
    // Each genus' foliage as flora::Foliage has it: its own colour, then the share of the
    // ground's.
    float4 foliage[genus_count];
    // Each genus' year, as render::GenusSeasons has it.
    float4 leafing[genus_count];
    float4 first[genus_count];
    float4 last[genus_count];
};

[[vk::binding(0, 0)]] ConstantBuffer<Settings> settings;
// A layer per biome.
[[vk::binding(1, 0)]] Sampler2DArray materials;
// When lit through the air.
[[vk::binding(2, 0)]] ConstantBuffer<Air> air;
[[vk::binding(3, 0)]] ConstantBuffer<Lighting> lighting;
[[vk::binding(4, 0)]] Sampler2D transmittance_table;
[[vk::binding(5, 0)]] Sampler2DArray aerial_volume;
// As cloud_march.slang's.
[[vk::binding(6, 0)]] Sampler2D cloud_shadow;
// The sea, as ocean.slang reads it, for how high it stands over the ground.
[[vk::binding(7, 0)]] ConstantBuffer<Waves> sea;
[[vk::binding(8, 0)]] Sampler2DArray shore_depths;
// When lit through the air, for what water mirrors.
[[vk::binding(9, 0)]] Sampler2D sky_view_table;

struct Vertex {
    [[vk::location(0)]] float3 position; // metres from the patch's origin
    [[vk::location(1)]] float3 normal;
    [[vk::location(2)]] uint biome;
    [[vk::location(3)]] float4 woods; // as terrain::Woods, each byte read 0 to 1
    [[vk::location(4)]] float palms;  // the last of them
    [[vk::location(5)]] float scar;   // as terrain::Cover's first byte, 0 to 1
    [[vk::location(6)]] uint blend_biome;
    [[vk::location(7)]] float blend; // blend_biome's share, 0 to 1
};

struct Ground {
    float4 position : SV_Position;
    float3 normal;
    float3 from_eye;
    float3 drawn; // from the eye, bent
    float height; // above sea level
    // How high the sea stands over its mean there, as ShoreLevel's mean and swash, and its turning.
    float3 shore;
    float4 turning;
    // Each biome's share, four to a vector: at most two at a corner, blended across a triangle.
    float4 biomes[(biome_count + 3) / 4];
    float4 woods;
    float palms;
    float scar;
};

// Ground no higher above sea level than this, by the metre it lies from the eye, is too near level
// with the sea for depth to tell which is in front, so it's drawn under it.
static const float level_with_sea = 1e-5;
// How far behind the sea's true surface such ground is put, relative to its depth, to stay behind
// the flat triangles the sea is drawn with.
static const float behind_sea = 1e-3;

// How many times back through a surge of the waves' groups the swash is looked for, and over how
// many metres of height its edge is blurred.
static const int wet_samples = 8;
static const float wet_edges = 0.05;
// How dark wet ground is, against dry.
static const float wet_darkening = 0.55;

// Shares below this aren't worth sampling for.
static const float least_biome = 1.0 / 256.0;

// A year that keeps every leaf green, for ground lit without the air's.
static const float4 midsummer = float4(24.0, 24.0, 0.0, 0.0);

// As planet::Biome::Lake.
static const uint lake_biome = 13;
// The mean square slope of water too far off to draw its waves: a light breeze's, as Cox and Munk
// measured it.
static const float far_water_roughness = 0.02;

static const uint pine = 0;
static const uint oak = 1;
static const uint palm = 3;

// Bare wood, as the trees': pine, oak, maple, then palm.
static const float3 woods[genus_count] = { float3(0.055, 0.045, 0.038), float3(0.07, 0.064, 0.056),
                                           float3(0.08, 0.075, 0.068), float3(0.085, 0.072, 0.055) };

// A crown as the stamps draw one, for the tree's height: how wide, how much of the height it
// fills, how much of the box about it its outline fills seen from aside, and how tall its genus
// stands, as flora::stature.
struct Crown {
    float width;
    float length;
    float fill;
    float height;
};

Crown crown_of(uint genus) {
    if (genus == pine) {
        const Crown pine_crown = { 0.51, 0.84, 0.5, 1.0 };
        return pine_crown;
    }
    if (genus == oak) {
        const Crown oak_crown = { 0.81, 0.7, 0.785, 0.85 };
        return oak_crown;
    }
    if (genus == palm) {
        // A starburst of fronds, open between them, on top of a bare stem.
        const Crown palm_crown = { 0.62, 0.3, 0.45, 1.0 };
        return palm_crown;
    }
    const Crown maple_crown = { 0.61, 0.75, 0.785, 0.9 };
    return maple_crown;
}

// Of what its leaves hid, a bare broadleaf's twigs still hide this much.
static const float twigs = 0.35;
// The stamps' own shading of their crowns, averaged, and their sides lit by less of the sky than
// ground facing up.
static const float crown_shade = 0.5;
// The most a grazing look's cotangent counts for.
static const float grazing = 20.0;

// A genus' crowns through the year, its trees' own dates spread early to late: their colour as
// `green` times their foliage plus `rest`, over `hiding`; and how much of a leafy crown's
// hiding they keep, averaged.
struct Leafed {
    float green;
    float3 rest;
    float hiding;
};

Leafed leafed(uint genus, float4 year) {
    Leafed leafed;
    leafed.green = 0.0;
    leafed.rest = float3(0.0);
    leafed.hiding = 0.0;
    for (int own = -1; own <= 1; ++own) {
        const Leaves leaves = leaves_at(settings.leafing[genus], 0.7 * float(own), year);
        // The autumn colour is linear in the green it turns from.
        const float3 turned = autumn_colour(float3(0.0), leaves.turned, settings.first[genus].rgb,
                                            settings.last[genus].rgb);
        const float bare = twigs * (1.0 - leaves.held);
        leafed.green += leaves.held * max(1.0 - (2.0 * leaves.turned), 0.0) * crown_shade;
        leafed.rest += (leaves.held * turned * crown_shade) + (woods[genus] * 0.8 * bare);
        leafed.hiding += leaves.held + bare;
    }
    return leafed;
}

[shader("vertex")]
Ground vertex_main(Vertex vertex) {
    Ground ground;
    // Both terms are small near the eye, where precision matters.
    const float3 from_eye = push.origin.xyz + vertex.position;
    const float3 drawn = bent(from_eye, push.bend_up, push.bend_radii.xy);
    ground.position = mul(push.clip_from_eye, float4(drawn, 1.0));
    ground.drawn = drawn;
    ground.normal = vertex.normal;
    ground.from_eye = from_eye;
    ground.height = height_above_sea(from_eye, push.bend_up, push.bend_radii.x);
    ground.shore = float3(0.0);
    ground.turning = float4(1.0, 0.0, 1.0, 0.0);
    if (settings.tiling.z > 0.0) {
        const ShoreLevel level =
            shore_level_at(sea, shore_floor(sea, shore_depths, from_eye), from_eye);
        ground.shore = float3(level.mean, level.swash);
        ground.turning = level.turning;
    }
    const uint biome = min(vertex.biome, biome_count - 1);
    for (uint group = 0; group < (biome_count + 3) / 4; ++group) {
        ground.biomes[group] = float4(0.0);
    }
    const uint other = min(vertex.blend_biome, biome_count - 1);
    ground.biomes[biome / 4][biome % 4] = 1.0 - vertex.blend;
    ground.biomes[other / 4][other % 4] += vertex.blend;
    ground.woods = vertex.woods;
    ground.palms = vertex.palms;
    ground.scar = vertex.scar;
    return ground;
}

ShoreLevel shore_level(Ground ground) {
    ShoreLevel level;
    level.mean = ground.shore.x;
    level.swash = ground.shore.yz;
    level.turning = ground.turning;
    return level;
}

// The level of the lake standing over the ground, as the sea's surface tells it at each pixel, or
// none. Lakes too small to show between the ground's corners far off are still found.
float lake_level(Ground ground) {
    if (settings.tiling.z <= 0.0) {
        return 0.0;
    }
    const float2 water = still_water_at(sea, shore_depths, ground.from_eye);
    return lake_over(water) ? water.x : 0.0;
}

// How high the water stands over sea level at the ground: a lake's level, where one stands over it,
// or else the sea's over its mean.
float sea_level(Ground ground) {
    const float lake = lake_level(ground);
    return lake > 0.0 ? lake : level_now(shore_level(ground));
}

// How much of the ground is a lake beyond the shore grids, which draw the lakes within them as the
// sea: its share of the lake's biome, sharpened to an edge half way, fading in over the grids' edge.
float far_lake(Ground ground) {
    if (settings.tiling.z <= 0.0) {
        return 0.0;
    }
    const float share = ground.biomes[lake_biome / 4][lake_biome % 4];
    const float edge = saturate(((share - 0.5) / max(fwidth(share), 1e-4)) + 0.5);
    return edge * (1.0 - shore_covering(sea, ground.from_eye));
}

// How high the eye stands over the water at the ground: never under a lake, which it's only taken
// to look down on.
float eye_above(Ground ground) {
    const float lake = lake_level(ground);
    return lake > 0.0 ? max(push.bend_up.w - lake, 0.0) : push.bend_up.w - sea_level(ground);
}

// How wet the ground is: 1 under a lake or the sea's mean, and above that from 1 where the swash
// covers it or has only just left it, drying over a surge of the waves' groups, to 0 where it
// hasn't run for longer or never does.
float wetness(Ground ground) {
    if (settings.tiling.z <= 0.0) {
        return 0.0;
    }
    const float lake = lake_level(ground);
    if (lake > 0.0) {
        return saturate(0.5 + ((lake - ground.height) / wet_edges));
    }
    const ShoreLevel level = shore_level(ground);
    const float highest = level.mean + level.swash.x + level.swash.y;
    float wet = saturate(0.5 + ((level.mean - ground.height) / wet_edges));
    if (level.swash.y <= 0.0 || ground.height >= highest) {
        return wet;
    }
    const float surge = 2.0 * pi / max(sea.run_up.w, 1e-3);
    for (int back = 0; back < wet_samples; ++back) {
        const float ago = float(back) * surge / float(wet_samples);
        const float over = swash_high(sea, level, ago) - ground.height;
        wet = max(wet, saturate(0.5 + (over / wet_edges)) * exp(-ago / surge));
    }
    return wet;
}

// Up from the planet's centre through the ground.
float3 up_at(Ground ground) {
    return normalize(ground.from_eye + (push.bend_up.xyz * (push.bend_radii.x + push.bend_up.w)));
}

// The ground's colour from its biomes' materials, each projected along the planet's axes as
// plane_shares has it, and mixed at two scales.
float3 albedo(Ground ground, float3 normal) {
    const float3 tiled = ground.from_eye + float3(push.bend_radii.zw, push.origin.w);
    const float3 planes = plane_shares(up_at(ground), normal);
    const float2 across[3] = { tiled.yz, tiled.zx, tiled.xy };

    // Gradients taken while every fragment is still running, as sampling skips biomes and planes.
    float2 at[6];
    float2 along_x[6];
    float2 along_y[6];
    for (uint plane = 0; plane < 3; ++plane) {
        for (uint scale = 0; scale < 2; ++scale) {
            const uint index = (plane * 2) + scale;
            at[index] = across[plane] / settings.tiling[scale];
            along_x[index] = ddx(at[index]);
            along_y[index] = ddy(at[index]);
        }
    }

    float3 colour = float3(0.0);
    // Unrolled, so the interpolated shares are indexed statically.
    [unroll]
    for (uint biome = 0; biome < biome_count; ++biome) {
        const float share = ground.biomes[biome / 4][biome % 4];
        if (share < least_biome) {
            continue;
        }
        float3 near = float3(0.0);
        float3 far = float3(0.0);
        for (uint plane = 0; plane < 3; ++plane) {
            if (planes[plane] == 0.0) {
                continue;
            }
            const uint index = plane * 2;
            near += planes[plane] * materials.SampleGrad(float3(at[index], biome), along_x[index],
                                                         along_y[index]).rgb;
            far += planes[plane] * materials.SampleGrad(float3(at[index + 1], biome),
                                                        along_x[index + 1], along_y[index + 1]).rgb;
        }
        // Each scale strays from the mean in its own way, so together they still average to it.
        colour += share * near * far / max(settings.means[biome].rgb, 1.0 / 1024.0);
    }
    return colour;
}

// Ground a fire left black: charcoal and ash, greening back as the scar fades.
static const float3 charcoal = float3(0.03, 0.027, 0.024);

float3 scorched(Ground ground, float3 colour) {
    return lerp(colour, charcoal, 0.9 * saturate(ground.scar));
}

// As terrain::woods_crowding and woods_tallest.
static const float woods_crowding = 2.0;
static const float woods_tallest = 32.0;

// The ground's colour with woods folded in beyond where the tree stamps shrink away: as many
// crowns as flora stands there, shaped and coloured as the stamps draw them, hiding the more of
// the ground the more grazing the look. The foliage is drawn from the ground's own colour.
float3 wooded(Ground ground, float3 bare, float4 year) {
    const float reach = settings.canopy.x;
    const float away = length(ground.from_eye);
    // Rising as the stamps shrink, so the two keep the woods' cover between them.
    const float fade = saturate((reach - away) / (0.2 * reach));
    const float band = 1.0 - (fade * fade);
    // Trees to the square metre.
    const float crowding = ground.woods.x * (woods_crowding / 100.0);
    if (reach <= 0.0 || band <= 0.0 || crowding <= 0.0) {
        return bare;
    }
    const float tall = ground.woods.y * woods_tallest;
    const float broadleaf = ground.woods.z;
    const float maple = ground.woods.w;
    const float others = 1.0 - ground.palms;
    const float shares[genus_count] = { others * (1.0 - broadleaf), others * broadleaf * (1.0 - maple),
                                        others * broadleaf * maple, ground.palms };
    const float steep = dot(-ground.from_eye / max(away, 1e-3), up_at(ground));
    const float aslant = min(sqrt(max(1.0 - (steep * steep), 0.0)) / max(steep, 1e-3), grazing);

    float depth = 0.0;
    float3 tint = float3(0.0);
    for (uint genus = 0; genus < genus_count; ++genus) {
        if (shares[genus] <= 0.0) {
            continue;
        }
        const Crown crown = crown_of(genus);
        const float height = tall * crown.height;
        const float width = height * crown.width;
        const float seen_area =
            (0.785 * width * width) + (crown.fill * width * height * crown.length * aslant);
        const Leafed kind = leafed(genus, year);
        const float hiding = crowding * shares[genus] * (kind.hiding / 3.0) * seen_area;
        const float3 foliage = (bare * settings.foliage[genus].w) + settings.foliage[genus].rgb;
        depth += hiding;
        tint += hiding * ((foliage * kind.green) + kind.rest) / max(kind.hiding, 1e-4);
    }
    return lerp(bare, tint / max(depth, 1e-6), (1.0 - exp(-depth)) * band);
}

// Where the eye, above the sea standing `level` over its mean, sees its surface towards `drawn`, if
// it does: on the faked sphere drawing bends sea level onto, raised by the level.
float3 sea_towards(float3 drawn, float level, out bool seen) {
    const float3 up = push.bend_up.xyz;
    const float eye_height = push.bend_up.w;
    const float faked = push.bend_radii.y;
    const float3 looking = normalize(drawn);
    // Along `looking` from the eye, the sphere is met where t^2 - 2kt + c = 0.
    const float k = (faked + eye_height) * dot(looking, -up);
    const float c = (eye_height - level) * ((2.0 * faked) + eye_height + level);
    const float discriminant = (k * k) - c;
    seen = eye_height > level && k > 0.0 && discriminant >= 0.0;
    // The nearer root, without cancelling.
    return looking * (c / (k + sqrt(max(discriminant, 0.0))));
}

// The depth at which the eye sees the sea's surface past `drawn`, standing `level` over its mean,
// if it does.
float sea_depth(float3 drawn, float level, out bool seen) {
    const float4 clip = mul(push.clip_from_eye, float4(sea_towards(drawn, level, seen), 1.0));
    return clip.z / clip.w;
}

// Where the line of sight to the ground leaves the air, and which way is up there: at the sea's
// surface over ground beneath it, as the deep water drawn in its place has it.
struct AirEnd {
    float3 at; // from the eye, bent
    float3 up;
};

AirEnd air_ends(Ground ground) {
    AirEnd end = { ground.drawn, up_at(ground) };
    if (settings.tiling.z > 0.0 && ground.height < sea_level(ground)) {
        bool seen;
        const float3 surface = sea_towards(ground.drawn, sea_level(ground), seen);
        if (seen) {
            end.at = surface;
            end.up = normalize(surface + (push.bend_up.xyz * (push.bend_radii.y + push.bend_up.w)));
        }
    }
    return end;
}

struct Shaded {
    float4 colour : SV_Target;
    float depth : SV_DepthLessEqual;
};

// The ground's colour lit by `on_ground` facing its normal and `on_water` facing up, each times
// pi, with water over it where it lies below the sea as high as it stands.
float3 shaded(Ground ground, float3 normal, float3 on_ground, float3 on_water, float4 year) {
    const float3 lit_ground = wooded(ground, scorched(ground, albedo(ground, normal)), year) *
                              on_ground * lerp(1.0, wet_darkening, wetness(ground));
    if (settings.tiling.z <= 0.0) {
        return lit_ground;
    }
    return through_water(lit_ground, on_water, sea_level(ground) - ground.height,
                         eye_above(ground),
                         length(ground.from_eye), dot(-normalize(ground.from_eye), up_at(ground)));
}

// Ground at or within depth precision of the sea's surface is put just behind it, which then wins.
float depth_of(Ground ground) {
    float depth = ground.position.z;
    if (settings.tiling.z > 0.0 &&
        ground.height - sea_level(ground) <= level_with_sea * length(ground.from_eye)) {
        bool seen;
        const float sea = sea_depth(ground.drawn, sea_level(ground), seen);
        if (seen) {
            depth = min(depth, sea * (1.0 - behind_sea));
        }
    }
    return depth;
}

[shader("fragment")]
Shaded fragment_main(Ground ground) {
    const float3 sun = push.sun.xyz;
    const float night = push.sun.w;
    const float3 normal = normalize(ground.normal);
    const float3 on_ground = night + ((1.0 - night) * saturate(dot(normal, sun)));
    const float3 on_water = night + ((1.0 - night) * saturate(dot(up_at(ground), sun)));
    Shaded result;
    result.colour = float4(saturate(shaded(ground, normal, on_ground, on_water, midsummer)), 1.0);
    result.depth = depth_of(ground);
    return result;
}

// Lit by sunlight through the air at the ground's own height and the sky's light at the eye, seen
// through the air between, and tone mapped.
[shader("fragment")]
Shaded fragment_air(Ground ground) {
    const float3 sun = push.sun.xyz;
    const float scale = lighting.sky_scale.w;
    const float far = far_lake(ground);
    const float3 normal = normalize(ground.normal);
    const float3 up = up_at(ground);
    const float3 clear =
        transmittance(air, transmittance_table, max(ground.height, 0.0) * scale, dot(up, sun));
    const AirEnd end = air_ends(ground);
    const Shade shade = shade_at(lighting, cloud_shadow, end.at);
    const float3 sunlight = through_cloud(shade, clear);
    const float3 sky = sky_under_cloud(lighting, shade, clear * saturate(dot(up, sun)));
    const float3 on_ground =
        (sunlight * saturate(dot(normal, sun))) + (sky * (0.5 + (0.5 * dot(normal, up))));
    // The water over ground beneath the sea, lit where the eye looks into it.
    float3 on_water = float3(0.0);
    if (ground.height < sea_level(ground) || eye_above(ground) < 0.0 || far > 0.0) {
        const float facing = saturate(dot(end.up, sun));
        const float3 clear_water = transmittance(air, transmittance_table, 0.0, dot(end.up, sun));
        on_water = (through_cloud(shade, clear_water) * facing) +
                   sky_under_cloud(lighting, shade, clear_water * facing);
    }
    float3 radiance = shaded(ground, normal, on_ground, on_water, lighting.year) / pi;
    if (far > 0.0) {
        // Too far to see into, a lake is its deep water's colour, mirroring the sky and the sun
        // as the sea does; facing up from the faked sphere the sea is drawn on.
        const float3 looking = normalize(ground.drawn);
        const float3 surface_up =
            normalize(ground.drawn + (push.bend_up.xyz * (push.bend_radii.y + push.bend_up.w)));
        const float reflected = reflected_at(looking, surface_up);
        const float3 sky = sky_mirrored(air, lighting, sky_view_table, transmittance_table, shade,
                                        reflect(looking, surface_up), push.bend_up.xyz, sun);
        const float3 glint =
            through_cloud(shade, transmittance(air, transmittance_table, 0.0,
                                               dot(surface_up, sun))) *
            glint_lobe(looking, surface_up, far_water_roughness, sun);
        const float3 water = (water_colour * on_water / pi) * (1.0 - reflected);
        radiance = lerp(radiance, water + (reflected * (sky + glint)), far);
    }

    const float4 clip = mul(push.clip_from_eye, float4(ground.drawn, 1.0));
    const float2 uv = ((clip.xy / clip.w) * 0.5) + 0.5;
    const float4 between =
        aerial_perspective(aerial_volume, uv, length(end.at) * scale, lighting.reach.x);
    Shaded result;
    result.colour =
        float4(tone_mapped((radiance * between.a) + between.rgb, lighting.sun_exposure.w), 1.0);
    result.depth = depth_of(ground);
    return result;
}
