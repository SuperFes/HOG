struct Push {
    float4x4 clip_from_planet;
    // xyz towards the sun, w how bright the night side stays.
    float4 sun;
    // Radians below the horizon the sun fades over.
    float twilight;
    // Radii per unit of the relief's heights.
    float relief;
    // Steps between the relief's texels across a face, one fewer than it has.
    float relief_steps;
};

[[vk::push_constant]] ConstantBuffer<Push> push;
[[vk::binding(0, 0)]] Sampler2DArray faces;
[[vk::binding(1, 0)]] Sampler2DArray relief;

struct Vertex {
    [[vk::location(0)]] float3 position;
    [[vk::location(1)]] float3 face_point;
    [[vk::location(2)]] float3 along_s;
    [[vk::location(3)]] float3 along_t;
};

struct Ground {
    float4 position : SV_Position;
    float3 up;
    float3 face_point;
    float3 along_s;
    float3 along_t;
};

// The relief's texels sit on the face's corners and edges, so neighbouring faces agree there.
float height(float2 st, float face) {
    const float2 at = ((st * push.relief_steps) + 0.5) / (push.relief_steps + 1.0);
    return relief.SampleLevel(float3(at, face), 0.0).r;
}

[shader("vertex")]
Ground vertex_main(Vertex vertex) {
    const float lift = 1.0 + (push.relief * height(vertex.face_point.xy, vertex.face_point.z));
    Ground ground;
    ground.position = mul(push.clip_from_planet, float4(vertex.position * lift, 1.0));
    ground.up = vertex.position;
    ground.face_point = vertex.face_point;
    ground.along_s = vertex.along_s;
    ground.along_t = vertex.along_t;
    return ground;
}

// As the simulation lights the ground: full above the horizon, easing to none through twilight.
float sunlight(float elevation) {
    const float share = saturate((elevation + push.twilight) / push.twilight);
    return share * share * (3.0 - (2.0 * share));
}

// Which way the lifted ground faces, from how its height changes a texel either way, or less at
// the face's edges.
float3 ground_normal(Ground ground, float3 up) {
    const float2 st = ground.face_point.xy;
    const float face = ground.face_point.z;
    const float step = 1.0 / max(push.relief_steps, 1.0);
    const float2 low = max(st - step, 0.0);
    const float2 high = min(st + step, 1.0);
    const float lift = 1.0 + (push.relief * height(st, face));
    const float rise_s = push.relief * (height(float2(high.x, st.y), face) -
                                        height(float2(low.x, st.y), face)) / (high.x - low.x);
    const float rise_t = push.relief * (height(float2(st.x, high.y), face) -
                                        height(float2(st.x, low.y), face)) / (high.y - low.y);
    return normalize(cross((ground.along_s * lift) + (up * rise_s),
                           (ground.along_t * lift) + (up * rise_t)));
}

[shader("fragment")]
float4 fragment_main(Ground ground) : SV_Target {
    const float3 colour = faces.Sample(ground.face_point).rgb;
    const float3 up = normalize(ground.up);
    const float3 sun = push.sun.xyz;
    const float sun_up = dot(up, sun);
    // Slopes take more or less of the sun than level ground would, which sets day and night; the
    // softening keeps the terminator from flaring.
    const float soften = 0.05;
    const float slope = min((saturate(dot(ground_normal(ground, up), sun)) + soften) /
                                (saturate(sun_up) + soften),
                            2.0);
    const float night = push.sun.w;
    const float day = sunlight(asin(clamp(sun_up, -1.0, 1.0))) * slope;
    return float4(saturate(colour * (night + ((1.0 - night) * day))), 1.0);
}
