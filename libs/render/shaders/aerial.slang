import atmosphere;
import weather;

[[vk::binding(0, 0)]] ConstantBuffer<Air> air;
[[vk::binding(1, 0)]] Sampler2D transmittance_table;
[[vk::binding(2, 0)]] Sampler2D scattering_table;
[[vk::binding(3, 0)]] ConstantBuffer<Lighting> lighting;
[[vk::binding(4, 0)]] [[vk::image_format("rgba16f")]] RWTexture2DArray<float4> volume;
// As client::SkyTexel.
[[vk::binding(5, 0)]] SamplerCube sky_before;
[[vk::binding(6, 0)]] SamplerCube sky_after;

static const uint aerial_steps = 12;
static const uint haze_steps = 16;
// Drawn metres along a view to the sky the haze is held over.
static const float haze_reach = 60000.0;

// Haze along a view, drawn, from the eye. Fog's too thin a layer to be held here; the clouds draw
// it.
struct Haze : IAlong {
    float3 view;
    float cos_sun;

    // At `along` of the air's metres.
    float4 at(float along, float3 sunlight, float sun_mu) {
        const float scale = lighting.sky_scale.w;
        const float3 up = lighting.up_height.xyz;
        const float radius = lighting.weather.x;
        const float faked = lighting.weather.y;
        const float eye_height = lighting.weather.w;
        const float3 from_eye = view * (along / scale);
        const float height = drawn_height(from_eye, up, eye_height, faked);
        const float3 direction = on_planet(from_eye, up, faked + eye_height, faked, radius);
        const SkyCell cell =
            sky_cell(lerp(sky_before.SampleLevel(direction, 0.0),
                          sky_after.SampleLevel(direction, 0.0), lighting.weather.z));
        const float extinction = haze_extinction(cell, height);
        if (extinction <= 0.0) {
            return float4(0.0);
        }
        // As if under a clear sky; it's dimmed under cloud with the air.
        const float3 lit = mist_light(sunlight, sun_mu, cos_sun, mist_above(cell, height),
                                      lighting.sky_scale.rgb);
        return float4(lit * (extinction / scale), extinction / scale);
    }
};

// Only the haze, out along `view` to haze_reach or the sea: what it adds, and the share it lets
// through.
float4 haze_only(Haze haze) {
    const float scale = lighting.sky_scale.w;
    const float3 up = lighting.up_height.xyz;
    const float3 sun = lighting.sun_exposure.xyz;
    const float faked = lighting.weather.y;
    const float eye_height = lighting.weather.w;
    float3 light = float3(0.0);
    float passed = 1.0;
    float before = 0.0;
    for (uint at = 0; at < haze_steps; ++at) {
        const float share = (at + 1.0) / haze_steps;
        const float after = haze_reach * share * share;
        const float step = after - before;
        const float along = before + (step * 0.5);
        before = after;
        const float3 from_eye = haze.view * along;
        const float height = drawn_height(from_eye, up, eye_height, faked);
        // The sea hides the sky beyond.
        if (height < 0.0) {
            break;
        }
        const float sun_mu = dot(normalize((up * (faked + eye_height)) + from_eye), sun);
        const float3 sunlight = transmittance(air, transmittance_table, height * scale, sun_mu);
        const float4 here = haze.at(along * scale, sunlight, sun_mu);
        const float through = exp(-here.w * step * scale);
        light += passed * within_step(here.xyz, float3(here.w), float3(through), step * scale);
        passed *= through;
    }
    return float4(light * (lighting.reach.y + lighting.reach.z), passed);
}

// Each texel holds the light the air and the haze add along its pixel's view out to its slice's
// end, and the share of light they let through, averaged over red, green and blue.
[shader("compute")]
[numthreads(8, 8, 1)]
void aerial_main(uint3 id: SV_DispatchThreadID) {
    if (any(id >= uint3(aerial_size, aerial_size, aerial_slices + 1))) {
        return;
    }
    const float2 clip = (((float2(id.xy) + 0.5) / aerial_size) * 2.0) - 1.0;
    const float4 far = mul(lighting.eye_from_clip, float4(clip, 1.0, 1.0));
    const float3 view = normalize(far.xyz / far.w);
    Haze haze;
    haze.view = view;
    haze.cos_sun = dot(view, lighting.sun_exposure.xyz);
    if (id.z == aerial_haze) {
        volume[id] = haze_only(haze);
        return;
    }
    const float3x3 local = frame_of(lighting.up_height.xyz);
    const float3 sun = mul(local, lighting.sun_exposure.xyz);
    const Scattered scattered =
        march(air, transmittance_table, scattering_table, lighting.up_height.w, mul(local, view),
              sun, aerial_slice_end(id.z, lighting.reach.x), aerial_steps, haze);
    const float3 passed = scattered.transmittance;
    // Under cloud, the air's lit by what comes through it.
    volume[id] = float4(scattered.light * (lighting.reach.y + lighting.reach.z),
                        (passed.r + passed.g + passed.b) / 3.0);
}
