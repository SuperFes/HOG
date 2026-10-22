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

// Light scattered towards the eye along a ray, and the share of what lies beyond that still
// reaches it.
public struct Scattered {
    public float3 light;
    public float3 transmittance;
};

// What else lies along a view through the air.
public interface IAlong {
    // At `along` of the air's metres from the eye, where the sun's light reaches through the air as
    // `sunlight` from `sun_mu` above the horizon: xyz the light scattered towards the eye per metre
    // of the air's, w the share of light lost per metre.
    public float4 at(float along, float3 sunlight, float sun_mu);
    // What any cloud lets through to `along` of the air's metres from the eye.
    public Shade shade(float along);
};

public struct OnlyAir : IAlong {
    public float4 at(float along, float3 sunlight, float sun_mu) {
        return float4(0.0);
    }
    public Shade shade(float along) {
        return unshaded();
    }
};

// As hog::atmosphere::Model::march: along `view` from the eye at `height`, with `sun` towards the
// sun, both unit in a frame in which up from the eye is +Z, to where the ray leaves the air or
// meets the ground, or to `distance` if that's nearer.
public Scattered march(Air air, Sampler2D transmittance_table, Sampler2D scattering_table,
                       float height, float3 view, float3 sun, float distance, uint steps) {
    return march(air, transmittance_table, scattering_table, height, view, sun, distance, steps,
                 OnlyAir());
}

// As march(), through `also` as well as the air.
public Scattered march<A : IAlong>(Air air, Sampler2D transmittance_table,
                                   Sampler2D scattering_table, float height, float3 view,
                                   float3 sun, float distance, uint steps, A also) {
    Scattered scattered;
    scattered.light = float3(0.0);
    scattered.transmittance = float3(1.0);
    float3 eye = float3(0.0, 0.0, air.radii.x + max(height, 0.0));
    float eye_height = max(height, 0.0);
    float mu = view.z;
    float entered = 0.0;
    // From above the air, start where the ray enters it.
    const float top = air.radii.y;
    if (eye.z > top) {
        const float entering = (eye.z * eye.z * mu * mu) - ((eye.z - top) * (eye.z + top));
        if (mu >= 0.0 || entering < 0.0) {
            return scattered;
        }
        const float along = (-eye.z * mu) - sqrt(entering);
        eye += view * along;
        distance -= along;
        entered = along;
        const float radius = length(eye);
        eye_height = radius - air.radii.x;
        mu = dot(eye / radius, view);
    }
    const float length = min(to_end(air, eye_height, mu), max(distance, 0.0));
    const float cos_angle = dot(view, sun);
    const float rayleigh = rayleigh_phase(cos_angle);
    const float mie = mie_phase(cos_angle, air.mie_ozone.y);
    const float3 up = normalize(eye);

    // Denser near the eye, where the air is too.
    float before = 0.0;
    for (uint at = 0; at < steps; ++at) {
        const float share = (at + 1.0) / steps;
        const float after = length * share * share;
        const float step = after - before;
        const float along = before + (step * 0.5);
        before = after;
        const float point_height = height_along(air, eye_height, mu, along);
        const float3 point_up = normalize((up * (air.radii.x + eye_height)) + (view * along));
        const float sun_mu = dot(point_up, sun);
        const Medium medium = medium_at(air, point_height);
        const float3 sunlight = transmittance(air, transmittance_table, point_height, sun_mu);
        const float4 other = also.at(entered + along, sunlight, sun_mu);
        const Shade shade = also.shade(entered + along);
        // What the cloud scatters through lights the air from all round, as the sky does.
        const float3 lit =
            (((medium.rayleigh * rayleigh) + (medium.mie * mie)) * sunlight * shade.sun) +
            (medium.scattering() *
             ((multiple_scattering(air, scattering_table, point_height, sun_mu) * shade.sky) +
              (sunlight * max(sun_mu, 0.0) * shade.scattered / pi))) +
            other.xyz;
        const float3 extinction = medium.extinction + other.w;
        const float3 through = exp(-extinction * step);
        scattered.light += scattered.transmittance * within_step(lit, extinction, through, step);
        scattered.transmittance *= through;
    }
    return scattered;
}

public static const uint sky_view_width = 192;
public static const uint sky_view_height = 108;

// Of views from `radius`, the least angle from straight up that meets the air: from above it, the
// views nearer straight up pass it by.
public float least_zenith(Air air, float radius) {
    return radius > air.radii.y ? pi - asin(air.radii.y / radius) : 0.0;
}

// Hillaire's sky-view mapping, from the eye at `height`: across, the view's azimuth from the sun's,
// finest towards it; down, from the zenith, or the least angle from it that meets the air, to the
// horizon and on to the nadir, finest at the horizon.
public float2 sky_view_uv(Air air, float height, float mu, float cos_azimuth) {
    const float radius = air.radii.x + height;
    const float beta = acos(clamp(sqrt(above_ground(air, height)) / radius, 0.0, 1.0));
    const float horizon_angle = pi - beta;
    const float least = least_zenith(air, radius);
    const float zenith_angle = acos(clamp(mu, -1.0, 1.0));
    float v;
    if (zenith_angle < horizon_angle) {
        v = 0.5 * (1.0 - sqrt(saturate((horizon_angle - zenith_angle) / (horizon_angle - least))));
    } else {
        v = 0.5 + (0.5 * sqrt(saturate((zenith_angle - horizon_angle) / beta)));
    }
    const float u = sqrt(saturate(0.5 - (0.5 * cos_azimuth)));
    return float2(texel_from_unit(u, sky_view_width), texel_from_unit(v, sky_view_height));
}

// The view a texel of the sky-view table is for, with the sun along +X.
public float3 sky_view_direction(Air air, float height, float2 uv) {
    const float radius = air.radii.x + height;
    const float beta = acos(clamp(sqrt(above_ground(air, height)) / radius, 0.0, 1.0));
    const float horizon_angle = pi - beta;
    const float least = least_zenith(air, radius);
    const float u = unit_from_texel(uv.x, sky_view_width);
    const float v = unit_from_texel(uv.y, sky_view_height);
    float zenith_angle;
    if (v < 0.5) {
        const float coord = 1.0 - (2.0 * v);
        zenith_angle = horizon_angle - ((horizon_angle - least) * coord * coord);
    } else {
        const float coord = (2.0 * v) - 1.0;
        zenith_angle = horizon_angle + (beta * coord * coord);
    }
    const float cos_azimuth = 1.0 - (2.0 * u * u);
    const float sin_azimuth = sqrt(saturate(1.0 - (cos_azimuth * cos_azimuth)));
    return float3(sin(zenith_angle) * cos_azimuth, sin(zenith_angle) * sin_azimuth,
                  cos(zenith_angle));
}

// A frame in which `up` is +Z, as march() takes directions in.
public float3x3 frame_of(float3 up) {
    const float3 east = normalize(abs(up.z) < 0.9 ? cross(float3(0.0, 0.0, 1.0), up)
                                                  : cross(float3(1.0, 0.0, 0.0), up));
    return float3x3(east, cross(up, east), up);
}

// How the air lights a frame, as render::Sky writes it.
public struct Lighting {
    public float4x4 eye_from_clip;
    // xyz up through the eye, w its height above the ground in the air's metres.
    public float4 up_height;
    // xyz towards the sun, w how much brighter the picture is than the light reaching it.
    public float4 sun_exposure;
    // rgb the sky's irradiance on level ground at the eye, w the air's metres to a metre drawn.
    public float4 sky_scale;
    // x the air's metres the aerial-perspective volume reaches out to; y and z of the light falling
    // on any cloud over the eye, the shares let straight through and scattered through, and any
    // mist over it as well.
    public float4 reach;
    // As drawn: x the planet's radius, y the faked radius drawing bends it onto, z how far the
    // weather has faded from its maps before to those after, w the eye's height above sea level.
    public float4 weather;
    // Of the cloud's shadow, as shadow.slang reads it: x the metres its map reaches from the eye
    // east, west, north and south, 0 for none; y the metres above sea level the map's laid at; z
    // of the light falling straight down, the share the mist over the eye lets through, and w the
    // haze alone, less any fog.
    public float4 shadow;
    // The year at the eye, as season.slang keeps it: x hours of daylight, y the longest day's, z 1
    // while the days lengthen.
    public float4 year;
};

// Of the light falling on any cloud over a place, what reaches it, as render::CloudShade has it.
public struct Shade {
    // Of the sun's light, the share let straight through.
    public float sun;
    // Of the sky's light, the share let through, straight or scattered.
    public float sky;
    // Of the sun's light, the share scattered through, lighting as the sky does.
    public float scattered;
    // Of the sky, the share seen clear of the cloud.
    public float open;
};

public Shade unshaded() {
    Shade shade;
    shade.sun = 1.0;
    shade.sky = 1.0;
    shade.scattered = 0.0;
    shade.open = 1.0;
    return shade;
}

// As any cloud and mist over the eye shade it.
public Shade shade_at_eye(Lighting lighting) {
    Shade shade;
    shade.sun = lighting.reach.y;
    shade.sky = lighting.reach.y + lighting.reach.z;
    shade.scattered = lighting.reach.z;
    shade.open = lighting.reach.y;
    return shade;
}

// Of `sunlight`, the sun's light as it falls clear, what comes straight through the cloud.
public float3 through_cloud(Shade shade, float3 sunlight) {
    return sunlight * shade.sun;
}

// The sky's light on level ground under the cloud, lit by it and by `level_sunlight`, the sun's
// light on level ground as it falls clear.
public float3 sky_under_cloud(Lighting lighting, Shade shade, float3 level_sunlight) {
    return (lighting.sky_scale.rgb * shade.sky) + (level_sunlight * shade.scattered);
}

// The sky water shows along `mirrored`, read from the eye's sky-view table as if seen from the eye,
// whose up is `up`: where cloud hides it, the cloud's underside, as bright as the light it lets
// through. Reflected down, a view meets another wave, as bright as the sky low down.
public float3 sky_mirrored(Air air, Lighting lighting, Sampler2D sky_view, Sampler2D table,
                           Shade shade, float3 mirrored, float3 up, float3 sun) {
    const float mu = max(dot(mirrored, up), 0.0);
    const float sun_mu = dot(sun, up);
    const float3 flat_view = mirrored - (up * mu);
    const float3 flat_sun = sun - (up * sun_mu);
    const float across = length(flat_view) * length(flat_sun);
    const float cos_azimuth = across > 1e-6 ? dot(flat_view, flat_sun) / across : 1.0;
    const float3 clear_sky =
        sky_view.SampleLevel(sky_view_uv(air, lighting.up_height.w, mu, cos_azimuth), 0.0).rgb;
    const float3 under =
        sky_under_cloud(lighting, shade, transmittance(air, table, 0.0, sun_mu) * max(sun_mu, 0.0)) /
        pi;
    return lerp(under, clear_sky, shade.open);
}

public static const uint aerial_size = 32;
public static const uint aerial_slices = 32;
// After the slices, a layer holding only what the haze adds and lets through along each view to the
// sky, as the sky's table leaves it out.
public static const uint aerial_haze = aerial_slices;

// The air's metres slice `slice` of the aerial-perspective volume reaches out to, finer near the
// eye.
public float aerial_slice_end(uint slice, float reach) {
    const float share = (slice + 1.0) / aerial_slices;
    return reach * share * share;
}

// What the air between the eye and what's drawn at `uv` on the picture, `distance` of the air's
// metres away, adds to the light from it, and as w the share of that light it lets through.
public float4 aerial_perspective(Sampler2DArray volume, float2 uv, float distance, float reach) {
    const float slice = (sqrt(max(distance, 0.0) / reach) * aerial_slices) - 1.0;
    const float below = floor(slice);
    const float4 far = volume.SampleLevel(float3(uv, clamp(below + 1.0, 0.0, aerial_slices - 1.0)), 0.0);
    const float4 near =
        below < 0.0 ? float4(0.0, 0.0, 0.0, 1.0)
                    : volume.SampleLevel(float3(uv, min(below, aerial_slices - 1.0)), 0.0);
    return lerp(near, far, slice >= aerial_slices - 1.0 ? 1.0 : slice - below);
}
