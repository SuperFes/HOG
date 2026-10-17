module shadow;

import atmosphere;
import weather;

// As clouds.cpp's: of the sun's light the mist takes, the share it scatters on down.
static const float mist_scattered_down = 0.7;
// Of the map's width, the share at each edge over which it fades to the shade over the eye.
static const float shadow_edge = 0.1;
// The sun's light is followed down to the map as if from no lower than this.
static const float least_sun = 0.05;
// Metres below the top of the cloud over a place over which it comes out of the cloud's shade.
static const float shadow_fade = 300.0;

// The metres east and north of the eye, `up` through it, that `direction` from the planet's centre
// is at as the map of the cloud's shadow lays the planet out, flat on the plane `radius` from the
// centre.
public float2 shadow_offset(float3 direction, float3 up, float radius) {
    const float3 local = mul(frame_of(up), direction);
    return local.z > 0.0 ? local.xy * (radius / local.z) : float2(1e30);
}

// As shadow_offset(), from `offset` back to the direction from the planet's centre.
public float3 shadow_direction(float2 offset, float3 up, float radius) {
    const float3x3 frame = frame_of(up);
    return normalize((up * radius) + (frame[0] * offset.x) + (frame[1] * offset.y));
}

// What any cloud lets through to what's drawn `from_eye`, from `map`, its shadow as
// cloud_march.slang's shadow_main() leaves it, as far as that reaches; as over the eye past it.
public Shade shade_at(Lighting lighting, Sampler2D map, float3 from_eye) {
    const Shade eye = shade_at_eye(lighting);
    const float reach = lighting.shadow.x;
    if (reach <= 0.0) {
        return eye;
    }
    const float3 up = lighting.up_height.xyz;
    const float radius = lighting.weather.x;
    const float faked = lighting.weather.y;
    const float eye_height = lighting.weather.w;
    const float3 sun = lighting.sun_exposure.xyz;
    const float height = drawn_height(from_eye, up, eye_height, faked);
    const float3 direction = on_planet(from_eye, up, faked + eye_height, faked, radius);
    // Back along the sun's light to the height the map's laid at.
    const float down = (height - lighting.shadow.y) / max(dot(direction, sun), least_sun);
    const float3 laid = (direction * (radius + height)) - (sun * down);
    const float2 uv = (shadow_offset(normalize(laid), up, radius) / (2.0 * reach)) + 0.5;
    const float2 edges = saturate(min(uv, 1.0 - uv) / shadow_edge);
    const float within = min(edges.x, edges.y);
    if (within <= 0.0) {
        return eye;
    }
    const float4 held = map.SampleLevel(uv, 0.0);
    // Over the cloud, out of its shade.
    const float under = saturate((held.a - height) / shadow_fade);
    const float sun_through = lerp(1.0, held.r, under);
    const float open = lerp(1.0, held.g, under);
    const float scattered = held.b * under;
    const float mist = lighting.shadow.z;
    Shade shade;
    shade.sun = lerp(eye.sun, sun_through * mist, within);
    shade.sky =
        lerp(eye.sky, (open * lerp(mist_scattered_down, 1.0, mist)) + scattered, within);
    shade.scattered = lerp(eye.scattered,
                           scattered + (sun_through * (1.0 - mist) * mist_scattered_down), within);
    shade.open = lerp(eye.open, open * mist, within);
    return shade;
}
