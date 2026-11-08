#include <hog/formation/climate.hpp>

#include <hog/core/portable_maths.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace hog::formation {

namespace {

// W/m² more radiated per °C warmer (Budyko).
constexpr double radiated_per_degree = 1.9;
constexpr int rows = 181; // latitudes from -90° to 90°, a degree apart

double asin(double x) noexcept {
    return maths::atan2(x, std::sqrt(std::max(0.0, 1.0 - (x * x))));
}

double acos(double x) noexcept {
    return maths::atan2(std::sqrt(std::max(0.0, 1.0 - (x * x))), x);
}

// Sunlight on the ground averaged over a day, W/m², the sun `declination` radians off the equator.
double daily_sunlight(double flux, double latitude, double declination) noexcept {
    const double sin_lat = maths::sin(latitude);
    const double cos_lat = maths::cos(latitude);
    const double sin_dec = maths::sin(declination);
    const double cos_dec = maths::cos(declination);
    const double across = cos_lat * cos_dec;
    double half_day = 0.0; // radians the sun is up either side of noon
    if (std::abs(across) < 1e-12) {
        half_day = sin_lat * sin_dec > 0.0 ? std::numbers::pi : 0.0;
    } else {
        half_day = acos(std::clamp(-(sin_lat * sin_dec) / across, -1.0, 1.0));
    }
    return (flux / std::numbers::pi) *
           ((half_day * sin_lat * sin_dec) + (across * maths::sin(half_day)));
}

std::array<double, rows> temperature_rows(const Body& body) {
    const double tilt = body.tilt * std::numbers::pi / 180.0;
    std::array<double, rows> sunlight{};
    std::array<double, rows> weight{};
    for (int row = 0; row < rows; ++row) {
        const double latitude = (row - 90) * std::numbers::pi / 180.0;
        constexpr int steps = 360;
        double sum = 0.0;
        for (int step = 0; step < steps; ++step) {
            const double season = (step + 0.5) * 2.0 * std::numbers::pi / steps;
            sum += daily_sunlight(body.star_flux, latitude,
                                  asin(maths::sin(tilt) * maths::sin(season)));
        }
        sunlight[static_cast<std::size_t>(row)] = sum / steps;
        weight[static_cast<std::size_t>(row)] = maths::cos(latitude);
    }
    std::array<double, rows> temperature{};
    std::array<double, rows> albedo{};
    albedo.fill(body.albedo);
    // Ice brightens where it's cold, which cools it further: settled in a few rounds.
    for (int round = 0; round < 12; ++round) {
        double mean = 0.0;
        double total = 0.0;
        for (std::size_t row = 0; row < rows; ++row) {
            mean += weight[row] * ((sunlight[row] * (1.0 - albedo[row])) - body.greenhouse) /
                    radiated_per_degree;
            total += weight[row];
        }
        mean /= total;
        for (std::size_t row = 0; row < rows; ++row) {
            temperature[row] = ((sunlight[row] * (1.0 - albedo[row])) - body.greenhouse +
                                (body.heat_transport * mean)) /
                               (radiated_per_degree + body.heat_transport);
            const double icy = std::clamp((-7.0 - temperature[row]) / 6.0, 0.0, 1.0);
            albedo[row] = body.albedo + ((body.ice_albedo - body.albedo) * icy);
        }
    }
    return temperature;
}

double read_rows(const std::array<double, rows>& table, double latitude) noexcept {
    const double at = std::clamp((latitude * 180.0 / std::numbers::pi) + 90.0, 0.0, 180.0);
    const auto below = std::min(static_cast<std::size_t>(at), std::size_t{rows - 2});
    const double share = at - static_cast<double>(below);
    return table[below] + ((table[below + 1] - table[below]) * share);
}

// How much water air can hold, against air at 15 °C (Clausius–Clapeyron, roughly).
double capacity(double celsius) noexcept {
    return maths::exp(0.07 * (celsius - 15.0));
}

} // namespace

double sea_level_temperature(const Body& body, double latitude) {
    return read_rows(temperature_rows(body), latitude);
}

Climate climate_on(const Sphere& sphere, std::span<const double> heights, const Body& body) {
    const std::uint32_t size = sphere.size();
    const std::array<double, rows> table = temperature_rows(body);
    Climate climate;
    climate.temperature.resize(size);
    climate.rain.resize(size);
    climate.wind.resize(size);

    // Air rises at the equator and every second cell boundary towards the poles and sinks between;
    // faster spin makes more, narrower cells. Surface winds turn east or west by cell.
    const double hours = std::max(std::abs(body.day), 0.1);
    const int cells = std::max(1, static_cast<int>(std::lround(3.0 * std::sqrt(24.0 / hours))));
    const double cell_width = 0.5 * std::numbers::pi / cells;
    const double turning = body.day < 0.0 ? -1.0 : 1.0;
    std::vector<double> rising(size);
    for (std::uint32_t point = 0; point < size; ++point) {
        const DVec3& here = sphere.point(point);
        const double latitude = asin(std::clamp(here.z, -1.0, 1.0));
        climate.temperature[point] =
            read_rows(table, latitude) - (body.lapse_rate * std::max(heights[point], 0.0));
        const double phase = std::abs(latitude) / cell_width;
        rising[point] = 0.5 * (1.0 + maths::cos(std::numbers::pi * phase));
        const auto cell = static_cast<int>(phase);
        const DVec3 pole{0.0, 0.0, 1.0};
        const DVec3 east_axis = glm::cross(pole, here);
        if (glm::length(east_axis) < 1e-9) {
            climate.wind[point] = DVec3{1.0, 0.0, 0.0};
            continue;
        }
        const DVec3 east = glm::normalize(east_axis);
        const DVec3 equatorwards = glm::cross(here, east) * (latitude > 0.0 ? -1.0 : 1.0);
        const bool first_of_pair = cell % 2 == 0;
        const DVec3 along = east * (first_of_pair ? -turning : turning);
        const DVec3 meridional = equatorwards * (first_of_pair ? 0.35 : -0.35);
        climate.wind[point] = glm::normalize(along + meridional);
    }

    // Moisture comes off the liquid and rides the wind inland, falling as it goes and as the air
    // is pushed up slopes; air that cools holds less, so the rest falls there too.
    std::vector<double> moisture(size);
    std::vector<double> lost(size, 0.0);
    for (std::uint32_t point = 0; point < size; ++point) {
        moisture[point] = heights[point] < 0.0 ? capacity(climate.temperature[point]) : 0.0;
    }
    std::vector<double> next = moisture;
    const int rounds =
        std::clamp(static_cast<int>(5.0 * body.rain_reach / sphere.spacing()), 20, 800);
    for (int round = 0; round < rounds; ++round) {
        for (std::uint32_t point = 0; point < size; ++point) {
            if (heights[point] < 0.0) {
                continue;
            }
            const DVec3& here = sphere.point(point);
            double weights = 0.0;
            double arriving = 0.0;
            double kept = 0.0;
            for (const std::uint32_t from : sphere.neighbors(point)) {
                const DVec3 step = here - sphere.point(from);
                const double length = glm::length(step);
                const double weight =
                    std::max(0.0, glm::dot(climate.wind[point], step / length));
                if (weight <= 0.0) {
                    continue;
                }
                const double rise = std::max(0.0, heights[point] - std::max(heights[from], 0.0));
                const double keep =
                    maths::exp(-(length / body.rain_reach) - (rise / body.vapour_height));
                weights += weight;
                arriving += weight * moisture[from];
                kept += weight * moisture[from] * keep;
            }
            if (weights <= 0.0) {
                next[point] = 0.0;
                lost[point] = 0.0;
                continue;
            }
            arriving /= weights;
            kept = std::min(kept / weights, capacity(climate.temperature[point]));
            next[point] = kept;
            lost[point] = arriving - kept;
        }
        std::swap(moisture, next);
    }

    double total = 0.0;
    double area = 0.0;
    for (std::uint32_t point = 0; point < size; ++point) {
        const double falling = heights[point] < 0.0
                                   ? moisture[point] * sphere.spacing() / body.rain_reach
                                   : lost[point];
        climate.rain[point] = (0.3 + rising[point]) * falling;
        total += climate.rain[point] * sphere.area(point);
        area += sphere.area(point);
    }
    const double scale = total > 0.0 ? body.rain_mean * area / total : 0.0;
    for (double& rain : climate.rain) {
        rain *= scale;
    }
    return climate;
}

} // namespace hog::formation
