struct Push {
    float4x4 clip_from_planet;
    // xyz towards the sun, w how bright the night side stays.
    float4 sun;
    // Radians below the horizon the sun fades over.
    float twilight;
};

[[vk::push_constant]] ConstantBuffer<Push> push;
[[vk::binding(0, 0)]] Sampler2DArray faces;

struct Vertex {
    [[vk::location(0)]] float3 position;
    [[vk::location(1)]] float3 face_point;
};

struct Ground {
    float4 position : SV_Position;
    float3 up;
    float3 face_point;
};

[shader("vertex")]
Ground vertex_main(Vertex vertex) {
    Ground ground;
    ground.position = mul(push.clip_from_planet, float4(vertex.position, 1.0));
    ground.up = vertex.position;
    ground.face_point = vertex.face_point;
    return ground;
}

// As the simulation lights the ground: full above the horizon, easing to none through twilight.
float sunlight(float elevation) {
    const float share = saturate((elevation + push.twilight) / push.twilight);
    return share * share * (3.0 - (2.0 * share));
}

[shader("fragment")]
float4 fragment_main(Ground ground) : SV_Target {
    const float3 colour = faces.Sample(ground.face_point).rgb;
    const float elevation = asin(clamp(dot(normalize(ground.up), push.sun.xyz), -1.0, 1.0));
    const float night = push.sun.w;
    return float4(colour * (night + ((1.0 - night) * sunlight(elevation))), 1.0);
}
