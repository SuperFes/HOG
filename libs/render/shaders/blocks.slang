import bend;

struct Push {
    float4x4 clip_from_eye;
    // xyz the blocks' origin from the eye.
    float4 origin;
    // xyz up through the eye; w the eye's height above sea level.
    float4 up_height;
    // xyz towards the sun; w the planet's radius.
    float4 sun_radius;
    // x the radius the ground is drawn curving like; y how bright sides away from the sun stay.
    float4 faked_night;
};

[[vk::push_constant]] ConstantBuffer<Push> push;

struct Vertex {
    [[vk::location(0)]] float3 position;
    [[vk::location(1)]] float3 normal;
    [[vk::location(2)]] float3 colour;
};

struct Surface {
    float4 position : SV_Position;
    float3 normal;
    float3 colour;
};

[shader("vertex")]
Surface vertex_main(Vertex vertex) {
    const float3 from_eye = vertex.position + push.origin.xyz;
    const float3 drawn =
        bent(from_eye, push.up_height, float2(push.sun_radius.w, push.faked_night.x));
    Surface surface;
    surface.position = mul(push.clip_from_eye, float4(drawn, 1.0));
    surface.normal = vertex.normal;
    surface.colour = vertex.colour;
    return surface;
}

[shader("fragment")]
float4 fragment_main(Surface surface) : SV_Target {
    const float night = push.faked_night.y;
    const float facing = saturate(dot(normalize(surface.normal), push.sun_radius.xyz));
    return float4(surface.colour * (night + ((1.0 - night) * facing)), 1.0);
}
