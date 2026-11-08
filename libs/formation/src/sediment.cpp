#include <hog/core/portable_maths.hpp>
#include <hog/formation/sediment.hpp>

#include <algorithm>
#include <numeric>

namespace hog::formation {

namespace {

// Lengths and areas are reckoned at Earth's scale, as the rivers' are.
constexpr double earth_radius = 6'371'000.0;

double angle(const DVec3& a, const DVec3& b) noexcept {
    return maths::atan2(glm::length(glm::cross(a, b)), glm::dot(a, b));
}

bool in_sea(const Rivers& rivers, std::uint32_t point) {
    return rivers.receiver[point] == point;
}

// Land points from the highest down, so each is reached after everything that runs into it.
std::vector<std::uint32_t> highest_first(const Rivers& rivers) {
    std::vector<std::uint32_t> order;
    for (std::uint32_t point = 0; point < rivers.receiver.size(); ++point) {
        if (!in_sea(rivers, point)) {
            order.push_back(point);
        }
    }
    std::ranges::sort(order, [&](std::uint32_t a, std::uint32_t b) {
        return rivers.height[a] > rivers.height[b];
    });
    return order;
}

// What a hollow keeps of what runs into it: all of it when nothing leaves, else as much as the
// water in it has room for over the time the land has had.
double kept_by(const Lakes& lakes, const Basin& basin, double arriving, const Body& body) {
    if (basin.lake == Lakes::none || !lakes.lakes[basin.lake].open) {
        return arriving;
    }
    return std::min(arriving, basin.room / std::max(body.sediment_age, 1.0));
}

struct Carried {
    std::vector<double> arriving; // cubic metres a year reaching each point from upstream
    std::vector<double> gathered; // cubic metres a year run into each hollow
    std::vector<double> kept;     // of which the hollow keeps
};

// Down the rivers from the highest ground, each point adding the rock worn off it and dropping
// what its river can't carry on; hollows gather all that runs into them and pass on what they
// can't keep from their outlet; at the coast the rest settles on the sea floor.
void carry(const Sphere& sphere, const Rivers& rivers, const Lakes& lakes, const Body& body,
           Sediment& sediment, Carried& carried) {
    const double cell_area = earth_radius * earth_radius;
    for (const std::uint32_t point : highest_first(rivers)) {
        const std::uint32_t basin = lakes.basin[point];
        double passing = carried.arriving[point];
        if (basin == Lakes::none) {
            passing += rivers.wear[point] * sphere.area(point) * cell_area;
            const std::uint32_t down = rivers.receiver[point];
            const double below = in_sea(rivers, down) ? 0.0 : rivers.height[down];
            const double slope = std::max(0.0, rivers.height[point] - below) /
                                 (angle(sphere.point(point), sphere.point(down)) * earth_radius);
            // Floods carry what floods cut, less the share of water lakes upstream have taken.
            const double kept_water = rivers.discharge[point] > 0.0
                                          ? lakes.discharge[point] / rivers.discharge[point]
                                          : 1.0;
            const double carrying =
                carrying_capacity(rivers.flood[point] * kept_water, slope, body);
            const double dropped = std::max(0.0, passing - carrying);
            sediment.settling[point] += dropped;
            passing -= dropped;
        } else {
            carried.gathered[basin] += passing;
            if (point != lakes.basins[basin].outlet) {
                continue;
            }
            const double kept = kept_by(lakes, lakes.basins[basin], carried.gathered[basin], body);
            carried.kept[basin] = kept;
            passing = carried.gathered[basin] - kept;
        }
        sediment.flux[point] = passing;
        const std::uint32_t down = rivers.receiver[point];
        if (in_sea(rivers, down)) {
            sediment.settling[down] += passing;
        } else {
            carried.arriving[down] += passing;
        }
    }
}

} // namespace

double carrying_capacity(double discharge, double slope, const Body& body) {
    return body.carrying * std::max(discharge, 0.0) * std::max(slope, 0.0);
}

Sediment sediment_on(const Sphere& sphere, const Rivers& rivers, const Lakes& lakes,
                     const Body& body) {
    const std::uint32_t size = sphere.size();
    Sediment sediment;
    sediment.flux.assign(size, 0.0);
    sediment.settling.assign(size, 0.0);
    sediment.alluvium.assign(size, 0.0);
    sediment.delta.assign(size, 0.0);
    sediment.lake_depth = lakes.depth;
    Carried carried{.arriving = std::vector<double>(size, 0.0),
                    .gathered = std::vector<double>(lakes.basins.size(), 0.0),
                    .kept = std::vector<double>(lakes.basins.size(), 0.0)};
    carry(sphere, rivers, lakes, body, sediment, carried);

    // A hollow spreads what it keeps over its floor; anything that reached it after its outlet
    // was passed (by a second way out) stays too. The water over it is that much shallower.
    std::vector<double> floor(lakes.basins.size(), 0.0);
    for (std::uint32_t point = 0; point < size; ++point) {
        if (lakes.basin[point] != Lakes::none) {
            floor[lakes.basin[point]] += sphere.area(point);
        }
    }
    for (std::uint32_t point = 0; point < size; ++point) {
        const std::uint32_t basin = lakes.basin[point];
        if (basin == Lakes::none) {
            continue;
        }
        const double late = carried.gathered[basin] - carried.kept[basin] -
                            sediment.flux[lakes.basins[basin].outlet];
        const double kept = carried.kept[basin] + std::max(0.0, late);
        sediment.settling[point] += kept * sphere.area(point) / floor[basin];
    }

    const double years = std::max(body.sediment_age, 0.0);
    for (std::uint32_t point = 0; point < size; ++point) {
        const double thickness =
            sediment.settling[point] * years / (sphere.area(point) * earth_radius * earth_radius);
        if (in_sea(rivers, point)) {
            sediment.delta[point] = thickness;
        } else {
            sediment.alluvium[point] = thickness;
            sediment.lake_depth[point] = std::max(0.0, lakes.depth[point] - thickness);
        }
    }
    return sediment;
}

} // namespace hog::formation
