#include <hog/core/portable_maths.hpp>
#include <hog/formation/lakes.hpp>

#include <algorithm>
#include <numeric>

namespace hog::formation {

namespace {

// Areas are reckoned at Earth's scale, as the rivers' discharge is.
constexpr double earth_radius = 6'371'000.0;

double smoothstep(double from, double to, double x) noexcept {
    const double t = std::clamp((x - from) / (to - from), 0.0, 1.0);
    return t * t * (3.0 - (2.0 * t));
}

// A hollow in the land: points the rivers filled deeper than the shallowest lake, joined up.
struct Hollow {
    std::vector<std::uint32_t> points;
    double spill = 0.0;       // the brim, where water would start to run out
    std::uint32_t outlet = 0; // the point it would run out from
};

std::vector<Hollow> find_hollows(const Sphere& sphere, const Rivers& rivers, const Body& body) {
    std::vector<std::uint32_t> owner(sphere.size(), Lakes::none);
    const auto deep = [&](std::uint32_t point) {
        return rivers.receiver[point] != point &&
               rivers.height[point] - rivers.ground[point] > body.shallowest_lake;
    };
    std::vector<Hollow> hollows;
    for (std::uint32_t first = 0; first < sphere.size(); ++first) {
        if (owner[first] != Lakes::none || !deep(first)) {
            continue;
        }
        const auto id = static_cast<std::uint32_t>(hollows.size());
        Hollow hollow{.points = {first}};
        owner[first] = id;
        for (std::size_t next = 0; next < hollow.points.size(); ++next) {
            for (const std::uint32_t other : sphere.neighbors(hollow.points[next])) {
                if (owner[other] == Lakes::none && deep(other)) {
                    owner[other] = id;
                    hollow.points.push_back(other);
                }
            }
        }
        hollows.push_back(std::move(hollow));
    }
    // Its brim is its highest filled surface; it runs out where the most water leaves it.
    for (Hollow& hollow : hollows) {
        double most = -1.0;
        hollow.spill = rivers.height[hollow.points.front()];
        hollow.outlet = hollow.points.front();
        for (const std::uint32_t point : hollow.points) {
            hollow.spill = std::max(hollow.spill, rivers.height[point]);
            const bool leaves = owner[rivers.receiver[point]] != owner[point];
            if (leaves && rivers.discharge[point] > most) {
                most = rivers.discharge[point];
                hollow.outlet = point;
            }
        }
    }
    return hollows;
}

// Takes `amount` of water out of every river downstream of `from`, down to the sea.
void take_downstream(const Rivers& rivers, std::vector<double>& discharge, std::uint32_t from,
                     double amount) {
    std::uint32_t at = from;
    while (rivers.receiver[at] != at) {
        at = rivers.receiver[at];
        discharge[at] = std::max(0.0, discharge[at] - amount);
    }
}

} // namespace

double open_water_loss(double celsius, const Body& body) {
    return body.lake_loss * maths::exp(0.06 * (celsius - 15.0)) * smoothstep(-15.0, 5.0, celsius);
}

Filling fill_hollow(std::span<const double> ground, std::span<const double> loss, double inflow,
                    double spill) {
    double taken = 0.0;
    for (std::size_t point = 0; point < ground.size(); ++point) {
        if (taken + loss[point] > inflow) {
            return {.covered = point, .level = ground[point]};
        }
        taken += loss[point];
    }
    return {.covered = ground.size(), .level = spill};
}

Lakes lakes_on(const Sphere& sphere, const Rivers& rivers, const Climate& climate,
               const Body& body) {
    Lakes lakes;
    lakes.depth.assign(sphere.size(), 0.0);
    lakes.lake.assign(sphere.size(), Lakes::none);
    lakes.basin.assign(sphere.size(), Lakes::none);
    lakes.salt_flat.assign(sphere.size(), 0);
    lakes.discharge = rivers.discharge;

    // Higher hollows first: what they keep back never reaches the ones below.
    std::vector<Hollow> hollows = find_hollows(sphere, rivers, body);
    std::ranges::sort(hollows, [](const Hollow& a, const Hollow& b) { return a.spill > b.spill; });
    for (Hollow& hollow : hollows) {
        std::ranges::sort(hollow.points, [&](std::uint32_t a, std::uint32_t b) {
            return rivers.ground[a] < rivers.ground[b];
        });
        std::vector<double> ground(hollow.points.size());
        std::vector<double> loss(hollow.points.size());
        for (std::size_t at = 0; at < hollow.points.size(); ++at) {
            const std::uint32_t point = hollow.points[at];
            ground[at] = rivers.ground[point];
            loss[at] = open_water_loss(climate.temperature[point], body) * sphere.area(point) *
                       earth_radius * earth_radius;
        }
        const double inflow = lakes.discharge[hollow.outlet];
        const Filling filling = fill_hollow(ground, loss, inflow, hollow.spill);
        const auto basin = static_cast<std::uint32_t>(lakes.basins.size());
        for (const std::uint32_t point : hollow.points) {
            lakes.basin[point] = basin;
        }
        lakes.basins.push_back({.outlet = hollow.outlet});
        if (filling.covered == 0) {
            lakes.salt_flat[hollow.points.front()] = 1;
            take_downstream(rivers, lakes.discharge, hollow.outlet, inflow);
            lakes.to_air += inflow;
            continue;
        }
        Lake lake{.level = filling.level,
                  .inflow = inflow,
                  .open = filling.covered == hollow.points.size(),
                  .outlet = hollow.outlet};
        const auto id = static_cast<std::uint32_t>(lakes.lakes.size());
        for (std::size_t at = 0; at < filling.covered; ++at) {
            const std::uint32_t point = hollow.points[at];
            lakes.depth[point] = std::max(0.0, filling.level - rivers.ground[point]);
            lakes.lake[point] = id;
            lake.area += sphere.area(point) * earth_radius * earth_radius;
            lake.loss += loss[at];
            lakes.basins.back().room +=
                lakes.depth[point] * sphere.area(point) * earth_radius * earth_radius;
        }
        lakes.basins.back().lake = id;
        take_downstream(rivers, lakes.discharge, hollow.outlet, std::min(inflow, lake.loss));
        lakes.to_air += std::min(inflow, lake.loss);
        lakes.lakes.push_back(lake);
    }
    return lakes;
}

} // namespace hog::formation
