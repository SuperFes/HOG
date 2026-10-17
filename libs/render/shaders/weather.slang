module weather;

// As client::SkyTexel's steps.
static const float fog_texel_step = 32.0;
static const float height_texel_step = 64.0;
// As weather_maps.cpp's. Haze grows as the air nears forming cloud, from none in dry air to about
// 4 km seen through at its wettest, and thins with height; fog lets about 260 m be seen through.
static const float haze_per_metre = 3e-5;
static const float haze_height = 1000.0;
static const float driest_share = 0.97;
static const float fog_per_metre = 0.015;
static const float fog_edge = 60.0;
// Fog lies under an inversion, above which the air's drier: the haze over it thins this quickly,
// fully over fog this deep.
static const float inversion = 150.0;
static const float inversion_fog = 100.0;
// Of light falling on the mist, the share scattered on, and how much of it forwards.
static const float mist_albedo = 0.95;
static const float mist_forward = 0.6;
// Of the light on level ground, as radiance from all round: the sky's above and the ground's below.
static const float ambient_share = 0.6;
// The sun's light through the mist above is found as if from no lower than this.
static const float least_sun = 0.05;

// The direction from the planet's true centre to what's drawn `from_eye`: as far along the ground
// as drawn, turned about the true centre.
public float3 on_planet(float3 from_eye, float3 up, float centre_to_eye, float faked,
                        float radius) {
    const float3 drawn_up = (up * centre_to_eye) + from_eye;
    const float3 across = from_eye - (up * dot(from_eye, up));
    const float out = length(across);
    const float turned = atan2(out, dot(drawn_up, up)) * (faked / radius);
    return (up * cos(turned)) + (out > 0.0 ? across * (sin(turned) / out) : float3(0.0));
}

// How high above the drawn sphere of radius `faked` what's drawn `from_eye` is, with the eye
// `eye_height` above it; stable near the eye.
public float drawn_height(float3 from_eye, float3 up, float eye_height, float faked) {
    const float along_up = dot(from_eye, up);
    const float out = length(from_eye - (up * along_up));
    const float above = along_up + eye_height;
    return ((2.0 * faked * above) + (above * above) + (out * out)) /
           (length(float2(faked + above, out)) + faked);
}

// The sky over a place as client::SkyTexel holds it, heights in metres above sea level.
public struct SkyCell {
    public float humidity;
    public float fog_top;
    public float base;
    public float top;
};

public SkyCell sky_cell(float4 texel) {
    SkyCell cell;
    cell.humidity = texel.r;
    cell.fog_top = texel.g * 255.0 * fog_texel_step;
    cell.base = texel.b * 255.0 * height_texel_step;
    cell.top = texel.a * 255.0 * height_texel_step;
    return cell;
}

// As weather_maps.cpp's: per metre at `height` above sea level, or at sea level below it.
public float haze_extinction(SkyCell cell, float height) {
    const float above = max(height, 0.0);
    const float over_fog = lerp(1.0, exp(-max(above - cell.fog_top, 0.0) / inversion),
                                saturate(cell.fog_top / inversion_fog));
    return haze_per_metre * ((1.0 / (1.0 - (driest_share * saturate(cell.humidity)))) - 1.0) *
           exp(-above / haze_height) * over_fog;
}

public float fog_extinction(SkyCell cell, float height) {
    return fog_per_metre * saturate((cell.fog_top - max(height, 0.0)) / fog_edge);
}

// As weather_maps.cpp's: how much of the light falling straight down from above the mist the mist
// over `height` takes.
public float mist_above(SkyCell cell, float height) {
    return (haze_extinction(cell, height) * haze_height) +
           (fog_per_metre * max(cell.fog_top - max(height, 0.0), 0.0));
}

// Of the mist at a point, under `above` of mist_above(), the light it scatters towards the eye for
// each share of light it takes: the sun's, `sunlight` as it reaches the mist from `sun_mu` above
// the horizon, as much as comes through the mist above, scattered forwards most; and the sky's,
// `sky` on level ground, and the sun's the mist above scattered, from all round.
public float3 mist_light(float3 sunlight, float sun_mu, float cos_sun, float above, float3 sky) {
    const float through = exp(-above / max(sun_mu, least_sun));
    const float3 direct = sunlight * through * henyey_greenstein(cos_sun, mist_forward);
    const float3 diffuse = ((sunlight * max(sun_mu, 0.0) * (1.0 - through)) + sky) *
                           (ambient_share / 3.14159265358979);
    return mist_albedo * (direct + diffuse);
}

// Henyey-Greenstein's.
public float henyey_greenstein(float cos_angle, float g) {
    const float g2 = g * g;
    return (1.0 - g2) / (4.0 * 3.14159265358979 * pow(1.0 + g2 - (2.0 * g * cos_angle), 1.5));
}
