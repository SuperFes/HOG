import bend;

struct Push {
    float4x4 clip_from_eye;
    // xyz towards the sun, w how bright ground facing away from it stays.
    float4 sun;
    // xyz up through the eye, w its height above sea level; then the planet's radius, the faked
    // one drawing bends it onto, and 1 if ground below sea level is seen through water.
    float4 bend_up;
    float4 bend_radii;
    // xyz the patch's origin from the eye, in metres.
    float4 origin;
};

[[vk::push_constant]] ConstantBuffer<Push> push;

static const uint biome_count = 14;

struct Palette {
    float4 colours[biome_count];
};

[[vk::binding(0, 0)]] ConstantBuffer<Palette> palette;

struct Vertex {
    [[vk::location(0)]] float3 position; // metres from the patch's origin
    [[vk::location(1)]] float3 normal;
    [[vk::location(2)]] uint biome;
};

struct Ground {
    float4 position : SV_Position;
    float3 normal;
    float3 colour;
    float3 from_eye;
    float height; // above sea level, or 0 if not seen through water
};

// Of clear seawater, by the metre of it light passes through, and the light it scatters back.
static const float3 water_absorbing = float3(0.35, 0.07, 0.03);
static const float3 water_colour = float3(0.0, 0.15, 0.25);

[shader("vertex")]
Ground vertex_main(Vertex vertex) {
    Ground ground;
    // Both terms are small near the eye, where precision matters.
    const float3 from_eye = push.origin.xyz + vertex.position;
    const float3 drawn = bent(from_eye, push.bend_up, push.bend_radii.xy);
    ground.position = mul(push.clip_from_eye, float4(drawn, 1.0));
    ground.normal = vertex.normal;
    ground.colour = palette.colours[min(vertex.biome, biome_count - 1)].rgb;
    ground.from_eye = from_eye;
    ground.height =
        push.bend_radii.z > 0.0 ? height_above_sea(from_eye, push.bend_up, push.bend_radii.x) : 0.0;
    return ground;
}

[shader("fragment")]
float4 fragment_main(Ground ground) : SV_Target {
    const float3 sun = push.sun.xyz;
    const float night = push.sun.w;
    const float3 lit_ground =
        ground.colour * (night + ((1.0 - night) * saturate(dot(normalize(ground.normal), sun))));
    const float under_sea = -ground.height;
    if (under_sea <= 0.0) {
        return float4(saturate(lit_ground), 1.0);
    }

    // Light down through the water and back up to the eye, the steeper the shorter.
    const float3 up = normalize(ground.from_eye +
                                (push.bend_up.xyz * (push.bend_radii.x + push.bend_up.w)));
    const float looking_down = max(dot(-normalize(ground.from_eye), up), 0.1);
    const float3 passed = exp(-water_absorbing * under_sea * (1.0 + (1.0 / looking_down)));
    const float3 lit_water = water_colour * (night + ((1.0 - night) * saturate(dot(up, sun))));
    return float4(saturate((lit_ground * passed) + (lit_water * (1.0 - passed))), 1.0);
}
