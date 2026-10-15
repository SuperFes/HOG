#include <hog/atmosphere/model.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>

namespace hog::atmosphere {

namespace {

constexpr double pi = std::numbers::pi;
// Fine enough that tabulating doesn't show, on the CPU and the GPU alike.
constexpr int transmittance_steps = 100;
constexpr int scattering_directions = 64;
constexpr int scattering_steps = 20;

// Squared distance from the planet's centre along a ray from `radius` at `mu`, less radius^2.
double discriminant(double radius, double mu, double sphere) noexcept {
    return (radius * radius * ((mu * mu) - 1.0)) + (sphere * sphere);
}

bool meets_ground(const Air& air, double radius, double mu) noexcept {
    return mu < 0.0 && discriminant(radius, mu, air.bottom) >= 0.0;
}

double to_ground(const Air& air, double radius, double mu) noexcept {
    return std::max((-radius * mu) - std::sqrt(std::max(discriminant(radius, mu, air.bottom), 0.0)),
                    0.0);
}

double to_top(const Air& air, double radius, double mu) noexcept {
    return std::max((-radius * mu) + std::sqrt(std::max(discriminant(radius, mu, air.top), 0.0)),
                    0.0);
}

double to_end(const Air& air, double radius, double mu) noexcept {
    return meets_ground(air, radius, mu) ? to_ground(air, radius, mu) : to_top(air, radius, mu);
}

double radius_along(double radius, double mu, double along) noexcept {
    return std::sqrt((along * along) + (2.0 * radius * mu * along) + (radius * radius));
}

// Texel centres span 0 to 1 of the unit range.
double unit_from_texel(double at, int size) noexcept {
    return (at - (0.5 / size)) / (1.0 - (1.0 / size));
}

double texel_from_unit(double at, int size) noexcept {
    return (0.5 / size) + (at * (1.0 - (1.0 / size)));
}

DVec3 bilinear(const std::vector<DVec3>& table, int width, int height, DVec2 uv) noexcept {
    const double x = std::clamp((uv.x * width) - 0.5, 0.0, width - 1.0);
    const double y = std::clamp((uv.y * height) - 0.5, 0.0, height - 1.0);
    const int x0 = std::min(static_cast<int>(x), width - 2);
    const int y0 = std::min(static_cast<int>(y), height - 2);
    const double s = x - x0;
    const double t = y - y0;
    const auto at = [&](int column, int row) {
        return table[(static_cast<std::size_t>(row) * static_cast<std::size_t>(width)) +
                     static_cast<std::size_t>(column)];
    };
    return glm::mix(glm::mix(at(x0, y0), at(x0 + 1, y0), s),
                    glm::mix(at(x0, y0 + 1), at(x0 + 1, y0 + 1), s), t);
}

// Of light along a step through a medium scattering `scattering` per metre, the share of what's
// scattered within the step that leaves it, for a step letting `passed` through.
DVec3 within_step(DVec3 scattering, DVec3 extinction, DVec3 passed, double length) noexcept {
    DVec3 result;
    for (glm::length_t channel = 0; channel < 3; ++channel) {
        result[channel] = extinction[channel] > 0.0
                              ? scattering[channel] * (1.0 - passed[channel]) / extinction[channel]
                              : scattering[channel] * length;
    }
    return result;
}

DVec3 exp(DVec3 value) noexcept {
    return {std::exp(value.x), std::exp(value.y), std::exp(value.z)};
}

} // namespace

Air Air::scaled_to(double radius) const noexcept {
    const double scale = radius / bottom;
    Air scaled = *this;
    scaled.bottom = radius;
    scaled.top = top * scale;
    scaled.rayleigh_scattering = rayleigh_scattering / scale;
    scaled.rayleigh_height = rayleigh_height * scale;
    scaled.mie_scattering = mie_scattering / scale;
    scaled.mie_extinction = mie_extinction / scale;
    scaled.mie_height = mie_height * scale;
    scaled.ozone_absorption = ozone_absorption / scale;
    scaled.ozone_peak = ozone_peak * scale;
    scaled.ozone_half_width = ozone_half_width * scale;
    return scaled;
}

Medium medium_at(const Air& air, double height) noexcept {
    const double rayleigh = std::exp(-height / air.rayleigh_height);
    const double mie = std::exp(-height / air.mie_height);
    const double ozone =
        std::max(1.0 - (std::abs(height - air.ozone_peak) / air.ozone_half_width), 0.0);
    return {.rayleigh = air.rayleigh_scattering * rayleigh,
            .mie = air.mie_scattering * mie,
            .extinction = (air.rayleigh_scattering * rayleigh) + DVec3{air.mie_extinction * mie} +
                          (air.ozone_absorption * ozone)};
}

DVec3 optical_depth(const Air& air, double height, double mu) {
    constexpr int steps = 4'000;
    const double radius = air.bottom + height;
    const double length = to_end(air, radius, mu);
    const double step = length / steps;
    DVec3 depth{0.0};
    for (int at = 0; at < steps; ++at) {
        const double along = (at + 0.5) * step;
        depth += medium_at(air, radius_along(radius, mu, along) - air.bottom).extinction * step;
    }
    return depth;
}

double rayleigh_phase(double cos_angle) noexcept {
    return 3.0 / (16.0 * pi) * (1.0 + (cos_angle * cos_angle));
}

// Cornette and Shanks', as Hillaire uses it.
double mie_phase(double cos_angle, double asymmetry) noexcept {
    const double g = asymmetry;
    const double scale = 3.0 / (8.0 * pi) * (1.0 - (g * g)) / (2.0 + (g * g));
    return scale * (1.0 + (cos_angle * cos_angle)) /
           std::pow(1.0 + (g * g) - (2.0 * g * cos_angle), 1.5);
}

// Bruneton's mapping: across heights by the distance to the horizon, and across directions by
// the distance to the top of the air, between its least and greatest from that height.
DVec2 transmittance_uv(const Air& air, double radius, double mu) noexcept {
    const double horizon = std::sqrt((air.top * air.top) - (air.bottom * air.bottom));
    const double rho = std::sqrt(std::max((radius * radius) - (air.bottom * air.bottom), 0.0));
    const double distance = to_top(air, radius, mu);
    const double least = air.top - radius;
    const double most = rho + horizon;
    return {texel_from_unit(most > least ? (distance - least) / (most - least) : 0.0,
                            transmittance_width),
            texel_from_unit(rho / horizon, transmittance_height)};
}

DVec2 scattering_uv(const Air& air, double height, double sun_mu) noexcept {
    return {
        texel_from_unit(std::clamp((sun_mu * 0.5) + 0.5, 0.0, 1.0), scattering_size),
        texel_from_unit(std::clamp(height / (air.top - air.bottom), 0.0, 1.0), scattering_size)};
}

Model::Model(const Air& air) : air_(air) {
    const double horizon = std::sqrt((air.top * air.top) - (air.bottom * air.bottom));
    transmittance_.reserve(static_cast<std::size_t>(transmittance_width) * transmittance_height);
    for (int row = 0; row < transmittance_height; ++row) {
        const double rho =
            horizon * unit_from_texel((row + 0.5) / transmittance_height, transmittance_height);
        const double radius = std::sqrt((rho * rho) + (air.bottom * air.bottom));
        for (int column = 0; column < transmittance_width; ++column) {
            const double least = air.top - radius;
            const double most = rho + horizon;
            const double distance =
                least + ((most - least) * unit_from_texel((column + 0.5) / transmittance_width,
                                                          transmittance_width));
            const double mu =
                distance == 0.0
                    ? 1.0
                    : std::clamp(((horizon * horizon) - (rho * rho) - (distance * distance)) /
                                     (2.0 * radius * distance),
                                 -1.0, 1.0);
            const double step = distance / transmittance_steps;
            DVec3 depth{0.0};
            for (int at = 0; at < transmittance_steps; ++at) {
                depth += medium_at(air, radius_along(radius, mu, (at + 0.5) * step) - air.bottom)
                             .extinction *
                         step;
            }
            transmittance_.push_back(exp(-depth));
        }
    }

    // Hillaire's approximation: light scattered twice, from every direction evenly, and the
    // share of light scattered again, summed as a geometric series.
    std::vector<DVec3> directions;
    directions.reserve(scattering_directions);
    const double golden = pi * (3.0 - std::sqrt(5.0));
    for (int at = 0; at < scattering_directions; ++at) {
        const double z = 1.0 - (((2.0 * at) + 1.0) / scattering_directions);
        const double across = std::sqrt(1.0 - (z * z));
        directions.emplace_back(across * std::cos(golden * at), across * std::sin(golden * at), z);
    }
    const double isotropic = 1.0 / (4.0 * pi);
    scattering_.reserve(static_cast<std::size_t>(scattering_size) * scattering_size);
    for (int row = 0; row < scattering_size; ++row) {
        const double height = (air.top - air.bottom) *
                              unit_from_texel((row + 0.5) / scattering_size, scattering_size);
        const double radius = air.bottom + height;
        for (int column = 0; column < scattering_size; ++column) {
            const double sun_mu =
                (2.0 * unit_from_texel((column + 0.5) / scattering_size, scattering_size)) - 1.0;
            const DVec3 sun{std::sqrt(1.0 - (sun_mu * sun_mu)), 0.0, sun_mu};
            DVec3 twice{0.0};
            DVec3 again{0.0};
            for (const DVec3& direction : directions) {
                const double mu = direction.z;
                const bool ground = meets_ground(air, radius, mu);
                const double length = to_end(air, radius, mu);
                const double step = length / scattering_steps;
                DVec3 passed{1.0};
                for (int at = 0; at < scattering_steps; ++at) {
                    const DVec3 point =
                        (DVec3{0.0, 0.0, radius}) + (direction * ((at + 0.5) * step));
                    const double point_radius = glm::length(point);
                    const Medium medium = medium_at(air, point_radius - air.bottom);
                    const DVec3 sunlit = transmittance(point_radius - air.bottom,
                                                       glm::dot(point / point_radius, sun));
                    const DVec3 through = exp(-medium.extinction * step);
                    twice += passed * within_step(medium.scattering() * sunlit * isotropic,
                                                  medium.extinction, through, step);
                    again +=
                        passed * within_step(medium.scattering(), medium.extinction, through, step);
                    passed *= through;
                }
                if (ground) {
                    const DVec3 point = (DVec3{0.0, 0.0, radius}) + (direction * length);
                    const double lit = glm::dot(glm::normalize(point), sun);
                    twice += passed * transmittance(0.0, lit) * std::max(lit, 0.0) *
                             air.ground_albedo / pi;
                }
            }
            twice /= static_cast<double>(scattering_directions);
            again /= static_cast<double>(scattering_directions);
            scattering_.push_back(twice / (DVec3{1.0} - again));
        }
    }
}

DVec3 Model::transmittance(double height, double mu) const noexcept {
    const double radius = std::clamp(air_.bottom + height, air_.bottom, air_.top);
    if (meets_ground(air_, radius, mu)) {
        return DVec3{0.0};
    }
    return bilinear(transmittance_, transmittance_width, transmittance_height,
                    transmittance_uv(air_, radius, mu));
}

DVec3 Model::multiple_scattering(double height, double sun_mu) const noexcept {
    return bilinear(scattering_, scattering_size, scattering_size,
                    scattering_uv(air_, height, sun_mu));
}

Scattered Model::march(double height, DVec3 view, DVec3 sun, double distance,
                       int steps) const noexcept {
    Scattered scattered;
    DVec3 eye{0.0, 0.0, air_.bottom + std::max(height, 0.0)};
    double radius = eye.z;
    double mu = view.z;
    // From above the air, start where the ray enters it.
    if (radius > air_.top) {
        const double entering = discriminant(radius, mu, air_.top);
        if (mu >= 0.0 || entering < 0.0) {
            return scattered;
        }
        const double along = (-radius * mu) - std::sqrt(entering);
        eye += view * along;
        distance -= along;
        radius = glm::length(eye);
        mu = glm::dot(eye / radius, view);
    }
    const double length = std::min(to_end(air_, radius, mu), std::max(distance, 0.0));
    const double cos_angle = glm::dot(view, sun);
    const double rayleigh = rayleigh_phase(cos_angle);
    const double mie = mie_phase(cos_angle, air_.mie_asymmetry);

    // Denser near the eye, where the air is too.
    double before = 0.0;
    for (int at = 0; at < steps; ++at) {
        const double after = length * ((at + 1.0) / steps) * ((at + 1.0) / steps);
        const double step = after - before;
        const DVec3 point = eye + (view * (before + (step * 0.5)));
        before = after;
        const double point_radius = glm::length(point);
        const double point_height = point_radius - air_.bottom;
        const double sun_mu = glm::dot(point / point_radius, sun);
        const Medium medium = medium_at(air_, point_height);
        const DVec3 lit = ((medium.rayleigh * rayleigh) + DVec3{medium.mie * mie}) *
                              transmittance(point_height, sun_mu) +
                          (medium.scattering() * multiple_scattering(point_height, sun_mu));
        const DVec3 through = exp(-medium.extinction * step);
        scattered.light +=
            scattered.transmittance * within_step(lit, medium.extinction, through, step);
        scattered.transmittance *= through;
    }
    return scattered;
}

} // namespace hog::atmosphere
