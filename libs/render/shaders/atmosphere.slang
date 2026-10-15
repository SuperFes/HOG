module atmosphere;

// As hog::atmosphere::Air, and its tables as hog::atmosphere::Model makes them. Heights are given
// rather than radii, so floats keep their precision near the ground.
public struct Air {
    // The ground's radius and the top of the air's, and the heights over which Rayleigh and Mie
    // scattering thin by e.
    public float4 radii;
    // xyz Rayleigh scattering, w Mie scattering.
    public float4 scattering;
    // Mie extinction and asymmetry, the height at which ozone is densest and how far either side it
    // thins to none.
    public float4 mie_ozone;
    public float4 ozone_absorption;
    public float4 ground_albedo;
};

public static const uint transmittance_width = 256;
public static const uint transmittance_height = 64;
public static const uint scattering_size = 32;

public static const float pi = 3.14159265358979;

public struct Medium {
    public float3 rayleigh;
    public float mie;
    public float3 extinction;

    public float3 scattering() {
        return rayleigh + mie;
    }
};

public Medium medium_at(Air air, float height) {
    const float rayleigh = exp(-height / air.radii.z);
    const float mie = exp(-height / air.radii.w);
    const float ozone = max(1.0 - (abs(height - air.mie_ozone.z) / air.mie_ozone.w), 0.0);
    Medium medium;
    medium.rayleigh = air.scattering.xyz * rayleigh;
    medium.mie = air.scattering.w * mie;
    medium.extinction =
        medium.rayleigh + (air.mie_ozone.x * mie) + (air.ozone_absorption.xyz * ozone);
    return medium;
}

public float rayleigh_phase(float cos_angle) {
    return 3.0 / (16.0 * pi) * (1.0 + (cos_angle * cos_angle));
}

public float mie_phase(float cos_angle, float g) {
    const float scale = 3.0 / (8.0 * pi) * (1.0 - (g * g)) / (2.0 + (g * g));
    return scale * (1.0 + (cos_angle * cos_angle)) / pow(1.0 + (g * g) - (2.0 * g * cos_angle), 1.5);
}

// radius^2 less the ground's, without cancelling.
float above_ground(Air air, float height) {
    return height * ((2.0 * air.radii.x) + height);
}

public bool meets_ground(Air air, float height, float mu) {
    const float radius = air.radii.x + height;
    return mu < 0.0 && (radius * radius * mu * mu) - above_ground(air, height) >= 0.0;
}

public float to_ground(Air air, float height, float mu) {
    const float radius = air.radii.x + height;
    const float discriminant = (radius * radius * mu * mu) - above_ground(air, height);
    // The nearer root, without cancelling.
    const float far = (-radius * mu) + sqrt(max(discriminant, 0.0));
    return far > 0.0 ? above_ground(air, height) / far : 0.0;
}

public float to_top(Air air, float height, float mu) {
    const float radius = air.radii.x + height;
    const float top = air.radii.y;
    const float discriminant = (radius * radius * mu * mu) + ((top - radius) * (top + radius));
    return max((-radius * mu) + sqrt(max(discriminant, 0.0)), 0.0);
}

public float to_end(Air air, float height, float mu) {
    return meets_ground(air, height, mu) ? to_ground(air, height, mu) : to_top(air, height, mu);
}

// Height above the ground `along` a ray from `height` at `mu`.
public float height_along(Air air, float height, float mu, float along) {
    const float radius = air.radii.x + height;
    const float squared = (along * along) + (2.0 * radius * mu * along) + above_ground(air, height);
    return squared / (sqrt(squared + (air.radii.x * air.radii.x)) + air.radii.x);
}

public float unit_from_texel(float at, uint size) {
    return (at - (0.5 / size)) / (1.0 - (1.0 / size));
}

public float texel_from_unit(float at, uint size) {
    return (0.5 / size) + (at * (1.0 - (1.0 / size)));
}

float horizon(Air air) {
    return sqrt((air.radii.y - air.radii.x) * (air.radii.y + air.radii.x));
}

public float2 transmittance_uv(Air air, float height, float mu) {
    const float rho = sqrt(above_ground(air, max(height, 0.0)));
    const float distance = to_top(air, height, mu);
    const float least = air.radii.y - air.radii.x - height;
    const float most = rho + horizon(air);
    return float2(texel_from_unit(most > least ? (distance - least) / (most - least) : 0.0,
                                  transmittance_width),
                  texel_from_unit(rho / horizon(air), transmittance_height));
}

// The height and direction a texel of the transmittance table is for.
public void transmittance_ray(Air air, float2 uv, out float height, out float mu) {
    const float H = horizon(air);
    const float rho = H * unit_from_texel(uv.y, transmittance_height);
    const float radius = sqrt((rho * rho) + (air.radii.x * air.radii.x));
    height = (rho * rho) / (radius + air.radii.x);
    const float least = air.radii.y - radius;
    const float most = rho + H;
    const float distance = least + ((most - least) * unit_from_texel(uv.x, transmittance_width));
    mu = distance == 0.0
             ? 1.0
             : clamp(((H * H) - (rho * rho) - (distance * distance)) / (2.0 * radius * distance),
                     -1.0, 1.0);
}

public float2 scattering_uv(Air air, float height, float sun_mu) {
    return float2(texel_from_unit(saturate((sun_mu * 0.5) + 0.5), scattering_size),
                  texel_from_unit(saturate(height / (air.radii.y - air.radii.x)), scattering_size));
}

// The share of light from the top of the air reaching `height` along `mu`, none where the ground
// is in the way.
public float3 transmittance(Air air, Sampler2D table, float height, float mu) {
    const float clamped = clamp(height, 0.0, air.radii.y - air.radii.x);
    if (meets_ground(air, clamped, mu)) {
        return float3(0.0);
    }
    return table.SampleLevel(transmittance_uv(air, clamped, mu), 0.0).rgb;
}

public float3 multiple_scattering(Air air, Sampler2D table, float height, float sun_mu) {
    return table.SampleLevel(scattering_uv(air, height, sun_mu), 0.0).rgb;
}

// Of light through a step letting `passed` through, the share of what's scattered within it that
// leaves it.
public float3 within_step(float3 scattering, float3 extinction, float3 passed, float length) {
    return select(extinction > 0.0, scattering * (1.0 - passed) / max(extinction, 1e-30),
                  scattering * length);
}
