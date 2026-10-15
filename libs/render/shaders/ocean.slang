struct Push {
    float4x4 clip_from_eye;
    // xyz up through the eye, w its height above sea level.
    float4 up_height;
    // xyz east along the ground under the eye, w the faked radius drawing bends the planet onto.
    float4 east_faked;
    // xyz towards the sun, w the metres the disc's rings start apart.
    float4 sun_nearest;
    // rgb the sky reflected, w the metres along the ground to the disc's rim.
    float4 sky_furthest;
};

[[vk::push_constant]] ConstantBuffer<Push> push;

struct Vertex {
    [[vk::location(0)]] float out; // from 0 under the eye to 1 at the rim
    [[vk::location(1)]] float2 towards; // east and north
};

struct Surface {
    float4 position : SV_Position;
    float3 from_eye;
    float3 normal;
};

// Reflected straight back, as water does.
static const float facing_reflection = 0.02;
static const float glint_sharpness = 2000.0;
static const float glint_brightness = 50.0;

// As hog::terrain::sea_along and sea_from_eye place it.
[shader("vertex")]
Surface vertex_main(Vertex vertex) {
    const float3 up = push.up_height.xyz;
    const float3 east = push.east_faked.xyz;
    const float faked = push.east_faked.w;
    const float nearest = push.sun_nearest.w;
    const float along = nearest * (pow((push.sky_furthest.w / nearest) + 1.0, vertex.out) - 1.0);

    const float3 towards = (east * vertex.towards.x) + (cross(up, east) * vertex.towards.y);
    const float turned = along / faked;
    const float half = sin(turned / 2.0);
    Surface surface;
    surface.from_eye =
        (up * ((-2.0 * faked * half * half) - push.up_height.w)) + (towards * (faked * sin(turned)));
    surface.position = mul(push.clip_from_eye, float4(surface.from_eye, 1.0));
    surface.normal = (up * cos(turned)) + (towards * sin(turned));
    return surface;
}

[shader("fragment")]
float4 fragment_main(Surface surface) : SV_Target {
    const float3 normal = normalize(surface.normal);
    const float3 looking = normalize(surface.from_eye);
    // From under the sea, its surface reflects as much back down.
    const float facing = abs(dot(looking, normal));
    const float reflected = facing_reflection + ((1.0 - facing_reflection) * pow(1.0 - facing, 5.0));

    const float3 sun = push.sun_nearest.xyz;
    const float3 halfway = normalize(sun - looking);
    const float lit = saturate(dot(normal, sun) * 10.0);
    const float glint =
        glint_brightness * reflected * lit * pow(saturate(dot(normal, halfway)), glint_sharpness);
    // Premultiplied: the sky by how much is reflected, and the glint over all.
    return float4((push.sky_furthest.rgb * reflected) + glint, reflected);
}
