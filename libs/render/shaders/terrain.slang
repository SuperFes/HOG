import bend;

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

struct Settings {
    // Metres a material spans near and far, and 1 if ground below sea level is seen through water.
    float4 tiling;
};

[[vk::binding(0, 0)]] ConstantBuffer<Settings> settings;
// A layer per biome.
[[vk::binding(1, 0)]] Sampler2DArray materials;

struct Vertex {
    [[vk::location(0)]] float3 position; // metres from the patch's origin
    [[vk::location(1)]] float3 normal;
    [[vk::location(2)]] uint biome;
};

struct Ground {
    float4 position : SV_Position;
    float3 normal;
    float3 from_eye;
    float3 drawn; // from the eye, bent
    float height; // above sea level, or 0 if not seen through water
    // Each biome's share, four to a vector: all of one at a corner, blended across a triangle.
    float4 biomes[(biome_count + 3) / 4];
};

// Of clear seawater, by the metre of it light passes through, and the light it scatters back.
static const float3 water_absorbing = float3(0.35, 0.07, 0.03);
static const float3 water_colour = float3(0.0, 0.15, 0.25);

// Ground no higher above sea level than this, by the metre it lies from the eye, is too near level
// with the sea for depth to tell which is in front, so it's drawn under it.
static const float level_with_sea = 1e-5;
// How far behind the sea's true surface such ground is put, relative to its depth, to stay behind
// the flat triangles the sea is drawn with.
static const float behind_sea = 1e-3;

// Shares below these aren't worth sampling for.
static const float least_biome = 1.0 / 256.0;
static const float least_plane = 0.02;

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
    ground.height =
        settings.tiling.z > 0.0 ? height_above_sea(from_eye, push.bend_up, push.bend_radii.x) : 0.0;
    const uint biome = min(vertex.biome, biome_count - 1);
    for (uint group = 0; group < (biome_count + 3) / 4; ++group) {
        ground.biomes[group] = float4(0.0);
    }
    ground.biomes[biome / 4][biome % 4] = 1.0;
    return ground;
}

// The ground's colour from its biomes' materials, each projected along the planet's axes, blended
// by how squarely the ground faces each, and mixed at two scales.
float3 albedo(Ground ground, float3 normal) {
    const float3 tiled = ground.from_eye + float3(push.bend_radii.zw, push.origin.w);
    float3 planes = pow(abs(normal), 4.0);
    planes = select(planes < least_plane * max(planes.x, max(planes.y, planes.z)), 0.0, planes);
    planes /= planes.x + planes.y + planes.z;
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
        const float3 mean = materials.SampleLevel(float3(0.5, 0.5, biome), 64.0).rgb;
        colour += share * near * far / max(mean, 1.0 / 1024.0);
    }
    return colour;
}

// The depth at which the eye, above sea level, sees the sea's surface past `drawn`, if it does: on
// the faked sphere drawing bends sea level onto.
float sea_depth(float3 drawn, out bool seen) {
    const float3 up = push.bend_up.xyz;
    const float eye_height = push.bend_up.w;
    const float faked = push.bend_radii.y;
    const float3 looking = normalize(drawn);
    // Along `looking` from the eye, the sphere is met where t^2 - 2kt + c = 0.
    const float k = (faked + eye_height) * dot(looking, -up);
    const float c = eye_height * ((2.0 * faked) + eye_height);
    const float discriminant = (k * k) - c;
    seen = eye_height > 0.0 && k > 0.0 && discriminant >= 0.0;
    // The nearer root, without cancelling.
    const float t = c / (k + sqrt(max(discriminant, 0.0)));
    const float4 clip = mul(push.clip_from_eye, float4(looking * t, 1.0));
    return clip.z / clip.w;
}

struct Shaded {
    float4 colour : SV_Target;
    float depth : SV_DepthLessEqual;
};

float3 shaded(Ground ground) {
    const float3 sun = push.sun.xyz;
    const float night = push.sun.w;
    const float3 normal = normalize(ground.normal);
    const float3 lit_ground =
        albedo(ground, normal) * (night + ((1.0 - night) * saturate(dot(normal, sun))));
    const float under_sea = -ground.height;
    if (under_sea <= 0.0) {
        return lit_ground;
    }

    // Light down through the water and back up to the eye, the steeper the shorter.
    const float3 up = normalize(ground.from_eye +
                                (push.bend_up.xyz * (push.bend_radii.x + push.bend_up.w)));
    const float looking_down = max(dot(-normalize(ground.from_eye), up), 0.1);
    const float3 passed = exp(-water_absorbing * under_sea * (1.0 + (1.0 / looking_down)));
    const float3 lit_water = water_colour * (night + ((1.0 - night) * saturate(dot(up, sun))));
    return (lit_ground * passed) + (lit_water * (1.0 - passed));
}

[shader("fragment")]
Shaded fragment_main(Ground ground) {
    Shaded result;
    result.colour = float4(saturate(shaded(ground)), 1.0);
    result.depth = ground.position.z;
    if (settings.tiling.z > 0.0 &&
        ground.height <= level_with_sea * length(ground.from_eye)) {
        bool seen;
        const float sea = sea_depth(ground.drawn, seen);
        if (seen) {
            result.depth = min(result.depth, sea * (1.0 - behind_sea));
        }
    }
    return result;
}
