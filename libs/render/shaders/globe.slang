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
    // Radii above the sphere the clouds and the top of the air lie.
    float cloud_lift;
    float air_lift;
    // xyz where the camera is, in radii from the planet's centre.
    float4 eye;
};

[[vk::push_constant]] ConstantBuffer<Push> push;
// The ground's colours and heights; the clouds' pipeline takes its own colours at 0 instead.
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

struct Shell {
    float4 position : SV_Position;
    float3 at; // in radii from the planet's centre
    float3 face_point;
};

Shell lift_shell(Vertex vertex, float lift) {
    Shell shell;
    shell.at = vertex.position * (1.0 + lift);
    shell.position = mul(push.clip_from_planet, float4(shell.at, 1.0));
    shell.face_point = vertex.face_point;
    return shell;
}

[shader("vertex")]
Shell cloud_vertex(Vertex vertex) {
    return lift_shell(vertex, push.cloud_lift);
}

// Colour, and how much of the sky the cloud covers as alpha, lit as the ground below is.
[shader("fragment")]
float4 cloud_fragment(Shell shell) : SV_Target {
    const float4 cloud = faces.Sample(shell.face_point);
    const float sun_up = dot(normalize(shell.at), push.sun.xyz);
    const float night = push.sun.w;
    const float light = night + ((1.0 - night) * sunlight(asin(clamp(sun_up, -1.0, 1.0))));
    return float4(cloud.rgb * light, cloud.a);
}

[shader("vertex")]
Shell air_vertex(Vertex vertex) {
    return lift_shell(vertex, push.air_lift);
}

// Where a ray from `origin` along unit `direction` enters and leaves a sphere about the centre,
// if it meets it.
bool crossing(float3 origin, float3 direction, float radius, out float enter, out float leave) {
    const float along = dot(origin, direction);
    const float miss = dot(origin, origin) - (along * along);
    const float half_chord_squared = (radius * radius) - miss;
    if (half_chord_squared < 0.0) {
        enter = 0.0;
        leave = 0.0;
        return false;
    }
    const float half_chord = sqrt(half_chord_squared);
    enter = -along - half_chord;
    leave = -along + half_chord;
    return true;
}

// The air between the camera and the far side of its shell, brighter the more of it a ray
// crosses and fading through a twilight wider than the ground's, since high air keeps the sun
// longer. Drawn on the shell's far side, so it shows from outside and in.
[shader("fragment")]
float4 air_fragment(Shell shell) : SV_Target {
    const float3 eye = push.eye.xyz;
    const float3 direction = normalize(shell.at - eye);
    const float top = 1.0 + push.air_lift;
    float enter;
    float leave;
    if (!crossing(eye, direction, top, enter, leave)) {
        return float4(0.0, 0.0, 0.0, 0.0);
    }
    enter = max(enter, 0.0);
    float ground_enter;
    float ground_leave;
    if (crossing(eye, direction, 1.0, ground_enter, ground_leave) && ground_enter > 0.0) {
        leave = min(leave, ground_enter);
    }
    const float longest = 2.0 * sqrt((top * top) - 1.0);
    const float depth = saturate((leave - enter) / longest);
    const float3 middle = eye + (direction * ((enter + leave) / 2.0));
    const float sun_up = dot(normalize(middle), push.sun.xyz);
    const float wide = 3.0 * push.twilight;
    const float share = saturate((asin(clamp(sun_up, -1.0, 1.0)) + wide) / wide);
    const float3 blue = float3(0.35, 0.6, 1.0);
    return float4(blue * depth * share * share * (3.0 - (2.0 * share)), 0.0);
}
