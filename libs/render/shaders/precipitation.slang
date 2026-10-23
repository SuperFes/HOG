import atmosphere;
import shadow;
import tone;
import water;

[[vk::binding(0, 0)]] ConstantBuffer<Air> air;
[[vk::binding(1, 0)]] ConstantBuffer<Lighting> lighting;
[[vk::binding(2, 0)]] Sampler2D transmittance_table;
// As cloud_march.slang's.
[[vk::binding(3, 0)]] Sampler2D cloud_shadow;

struct Push {
    float4x4 clip_from_eye;
    // xyz where the eye is among the drops, within the box they repeat in; w the box's side.
    float4 rain_shift_box;
    // xyz where the eye is among the flakes; w the share of what falls that's flakes.
    float4 snow_shift_share;
    // xyz the drops' velocity, w the radians across a pixel.
    float4 rain_velocity_pixel;
    // xyz the flakes' velocity, w the seconds within their sway's period.
    float4 snow_velocity_time;
};

[[vk::push_constant]] ConstantBuffer<Push> push;

static const float drop_radius = 0.001;    // metres
static const float flake_radius = 0.004;   // metres
static const float streak_seconds = 0.02;  // a drop moves this long while the eye takes it in
static const float sway = 0.15;            // metres a flake sways either way
static const float drop_opacity = 0.3;
static const float flake_opacity = 0.9;
static const float flake_albedo = 0.9;
static const float nearest = 0.3;          // metres, nearer than which they fade out

struct Particle {
    float4 position : SV_Position;
    noperspective float2 clip;
    float2 corner; // -1 to 1 across, and 0 at the head to 1 at the tail
    nointerpolation float opacity;
    nointerpolation uint flake;
};

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

// The two triangles of a quad, as corners -1 to 1.
static const float2 corners[6] = { float2(-1.0, -1.0), float2(1.0, -1.0), float2(1.0, 1.0),
                                   float2(-1.0, -1.0), float2(1.0, 1.0), float2(-1.0, 1.0) };

[shader("vertex")]
Particle vertex_main(uint vertex: SV_VertexID, uint instance: SV_InstanceID) {
    const float box = push.rain_shift_box.w;
    const float3 seed = float3(unit(instance * 4u), unit((instance * 4u) + 1u),
                               unit((instance * 4u) + 2u));
    const bool flake = unit((instance * 4u) + 3u) < push.snow_shift_share.w;
    const float3 shift = flake ? push.snow_shift_share.xyz : push.rain_shift_box.xyz;
    // Each repeats every box, about the eye.
    float3 at = ((seed * box) - shift);
    at = (at - (box * floor(at / box))) - (0.5 * box);
    const float3 up = lighting.up_height.xyz;
    if (flake) {
        const float3 east = normalize(abs(up.z) < 0.9 ? cross(float3(0.0, 0.0, 1.0), up)
                                                      : cross(float3(1.0, 0.0, 0.0), up));
        const float phase = 6.2831853 * seed.x;
        const float swayed = 3.14159265 * push.snow_velocity_time.w;
        at += sway * ((east * sin(swayed + phase)) + (cross(up, east) * cos(swayed + phase)));
    }

    const float distance = max(length(at), 1e-3);
    const float pixel = push.rain_velocity_pixel.w * distance;
    const float2 corner = corners[vertex];
    float3 placed;
    float coverage;
    if (flake) {
        // Facing the eye, and at least a pixel across, fainter for being smaller than that.
        const float half = max(flake_radius, 0.75 * pixel);
        coverage = (flake_radius / half) * (flake_radius / half);
        const float3 towards = at / distance;
        const float3 side = normalize(abs(dot(towards, up)) < 0.99 ? cross(towards, up)
                                                                   : cross(towards, float3(1.0, 0.0, 0.0)));
        const float3 over = cross(side, towards);
        placed = at + (half * ((side * corner.x) + (over * corner.y)));
    } else {
        // A streak along the drop's way while the eye takes it in, a pixel wide at least.
        const float3 along = -push.rain_velocity_pixel.xyz * streak_seconds;
        const float half = max(drop_radius, 0.5 * pixel);
        coverage = drop_radius / half;
        const float3 across = cross(normalize(along), at);
        const float3 side = length(across) > 1e-6 ? normalize(across) : float3(1.0, 0.0, 0.0);
        placed = at + (along * ((corner.y * 0.5) + 0.5)) + (side * (half * corner.x));
    }
    const float reach = 0.5 * box;
    const float fade = saturate((reach - distance) / (0.3 * reach)) *
                       saturate((distance - nearest) / nearest);

    Particle particle;
    particle.position = mul(push.clip_from_eye, float4(placed, 1.0));
    particle.clip = particle.position.xy / particle.position.w;
    particle.corner = float2(corner.x, (corner.y * 0.5) + 0.5);
    particle.opacity = coverage * fade * (flake ? flake_opacity : drop_opacity);
    particle.flake = flake ? 1u : 0u;
    return particle;
}

[shader("fragment")]
float4 fragment_main(Particle particle) : SV_Target {
    // Falling into the sea, it's gone.
    if (seen_from_water(lighting, particle.clip)) {
        discard;
    }
    const float3 sun = lighting.sun_exposure.xyz;
    const float3 up = lighting.up_height.xyz;
    const float sun_mu = dot(sun, up);
    const float3 clear =
        transmittance(air, transmittance_table, lighting.up_height.w, sun_mu) * max(sun_mu, 0.0);
    // Near enough the eye to be shaded as it is.
    const Shade shade = shade_at(lighting, cloud_shadow, float3(0.0));
    const float3 sunlight = through_cloud(shade, clear);
    const float3 sky = sky_under_cloud(lighting, shade, clear);
    float3 radiance;
    float shape;
    if (particle.flake != 0u) {
        // Lit from all round, as a tumbling flake is on average.
        radiance = flake_albedo * ((0.5 * sunlight) + sky) / pi;
        shape = saturate(1.0 - length(float2(particle.corner.x, (particle.corner.y * 2.0) - 1.0)));
    } else {
        // A drop shows the sky round it, bent through it.
        radiance = (sky / pi) + (0.05 * sunlight);
        shape = (1.0 - abs(particle.corner.x)) * sin(3.14159265 * particle.corner.y);
    }
    const float opacity = particle.opacity * shape;
    if (opacity <= 0.0) {
        discard;
    }
    return float4(tone_mapped(radiance, lighting.sun_exposure.w) * opacity, opacity);
}
