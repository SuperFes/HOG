import bend;

struct Push {
    float4x4 clip_from_eye;
    // xyz towards the sun, w how bright ground facing away from it stays.
    float4 sun;
    // xyz up through the eye, w its height above sea level; then the planet's radius and the
    // faked one drawing bends it onto.
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
};

[shader("vertex")]
Ground vertex_main(Vertex vertex) {
    Ground ground;
    // Both terms are small near the eye, where precision matters.
    const float3 from_eye = push.origin.xyz + vertex.position;
    const float3 drawn = bent(from_eye, push.bend_up, push.bend_radii.xy);
    ground.position = mul(push.clip_from_eye, float4(drawn, 1.0));
    ground.normal = vertex.normal;
    ground.colour = palette.colours[min(vertex.biome, biome_count - 1)].rgb;
    return ground;
}

[shader("fragment")]
float4 fragment_main(Ground ground) : SV_Target {
    const float facing = saturate(dot(normalize(ground.normal), push.sun.xyz));
    const float night = push.sun.w;
    return float4(saturate(ground.colour * (night + ((1.0 - night) * facing))), 1.0);
}
