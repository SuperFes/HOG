#include <hog/core/portable_maths.hpp>
#include <hog/formation/sediment.hpp>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <numeric>
#include <queue>
#include <utility>
#include <vector>

namespace hog::formation {

namespace {

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
    std::ranges::stable_sort(order, [&](std::uint32_t a, std::uint32_t b) {
        return rivers.height[a] > rivers.height[b];
    });
    return order;
}

// What a hollow keeps of what runs into it: as much as it has room for up to its brim over the
// time the land has had; a hollow filled to its brim passes the rest on, wet or dry.
double kept_by(const Basin& basin, double arriving, const Body& body) {
    return std::min(arriving, basin.room / std::max(body.sediment_age, 1.0));
}

struct Carried {
    std::vector<double> arriving; // cubic metres a year reaching each point from upstream
    std::vector<double> gathered; // cubic metres a year run into each hollow
    std::vector<double> kept;     // of which the hollow keeps
};

// Of `passing` cubic metres a year, what a river drops at a point: what it can't carry, but no
// more than builds its bed up steep enough to carry the rest over the time the land has had, as a
// river laying down more than it carries steepens until it carries what comes.
double dropped_at(const Sphere& sphere, const Rivers& rivers, const Lakes& lakes, const Body& body,
                  std::uint32_t point, double passing) {
    const std::uint32_t down = rivers.receiver[point];
    const double below = in_sea(rivers, down) ? 0.0 : rivers.height[down];
    const double length = angle(sphere.point(point), sphere.point(down)) * body.radius;
    const double slope = std::max(0.0, rivers.height[point] - below) / length;
    // Floods carry what floods cut, less the share of water lakes upstream have taken.
    const double kept_water =
        rivers.discharge[point] > 0.0 ? lakes.discharge[point] / rivers.discharge[point] : 1.0;
    const double flow = rivers.flood[point] * kept_water;
    const double excess = std::max(0.0, passing - carrying_capacity(flow, slope, body));
    if (excess <= 0.0 || flow * body.carrying <= 0.0) {
        return excess;
    }
    const double graded = passing / (body.carrying * flow);
    const double built = std::max(0.0, graded - slope) * length * sphere.area(point) * body.radius *
                         body.radius / std::max(body.sediment_age, 1.0);
    return std::min(excess, built);
}

// Down the rivers from the highest ground, each point adding the rock worn off it and dropping
// what its river can't carry on; hollows gather all that runs into them and pass on what they
// can't keep from their outlet; at the coast the rest settles on the sea floor.
void carry(const Sphere& sphere, const Rivers& rivers, const Lakes& lakes, const Body& body,
           Sediment& sediment, Carried& carried) {
    const double cell_area = body.radius * body.radius;
    for (const std::uint32_t point : highest_first(rivers)) {
        const std::uint32_t basin = lakes.basin[point];
        double passing = carried.arriving[point];
        if (basin == Lakes::none) {
            passing += rivers.wear[point] * sphere.area(point) * cell_area;
            const double dropped = dropped_at(sphere, rivers, lakes, body, point, passing);
            sediment.settling[point] += dropped;
            passing -= dropped;
        } else {
            carried.gathered[basin] += passing;
            if (point != lakes.basins[basin].outlet) {
                continue;
            }
            const double kept = kept_by(lakes.basins[basin], carried.gathered[basin], body);
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

// What settles in the sea over `years`, laid from each river mouth outwards over the sea floor,
// nearest first, each point filled no higher than the waterline: a delta builds out over the
// shelf rather than piling up where the river meets the sea.
void spread_deltas(const Sphere& sphere, const Rivers& rivers, double years, double radius,
                   Sediment& sediment) {
    const std::uint32_t size = sphere.size();
    const double cell_area = radius * radius;
    std::vector<double> room(size, 0.0);
    for (std::uint32_t point = 0; point < size; ++point) {
        if (in_sea(rivers, point)) {
            room[point] = std::max(0.0, -rivers.height[point]) * sphere.area(point) * cell_area;
        }
    }
    std::vector<double> laid(size, 0.0);
    std::vector<double> distance(size, std::numeric_limits<double>::infinity());
    std::vector<std::uint32_t> touched;
    using Entry = std::pair<double, std::uint32_t>;
    for (std::uint32_t mouth = 0; mouth < size; ++mouth) {
        double left = in_sea(rivers, mouth) ? sediment.settling[mouth] * years : 0.0;
        if (left <= 0.0) {
            continue;
        }
        std::priority_queue<Entry, std::vector<Entry>, std::greater<>> open;
        distance[mouth] = 0.0;
        touched.push_back(mouth);
        open.emplace(0.0, mouth);
        while (!open.empty() && left > 0.0) {
            const auto [far, point] = open.top();
            open.pop();
            if (far > distance[point]) {
                continue;
            }
            const double taken = std::min(left, room[point]);
            laid[point] += taken;
            room[point] -= taken;
            left -= taken;
            for (const std::uint32_t next : sphere.neighbors(point)) {
                const double there = far + angle(sphere.point(point), sphere.point(next));
                if (in_sea(rivers, next) && there < distance[next]) {
                    distance[next] = there;
                    touched.push_back(next);
                    open.emplace(there, next);
                }
            }
        }
        // A sea filled to its brim keeps the rest at the mouth.
        laid[mouth] += left;
        for (const std::uint32_t point : touched) {
            distance[point] = std::numeric_limits<double>::infinity();
        }
        touched.clear();
    }
    for (std::uint32_t point = 0; point < size; ++point) {
        sediment.delta[point] = laid[point] / (sphere.area(point) * cell_area);
    }
}

} // namespace

double carrying_capacity(double discharge, double slope, const Body& body) {
    return body.carrying * std::max(discharge, 0.0) * std::max(slope, 0.0);
}

Sediment sediment_on(const Sphere& sphere, const Rivers& rivers, const Lakes& lakes,
                     const Body& body) {
    return sediment_on(sphere, rivers, lakes, {}, body);
}

Sediment sediment_on(const Sphere& sphere, const Rivers& rivers, const Lakes& lakes,
                     std::span<const double> brought, const Body& body) {
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
    for (std::uint32_t point = 0; point < brought.size() && point < size; ++point) {
        const double yearly = brought[point] / std::max(body.sediment_age, 1.0);
        (in_sea(rivers, point) ? sediment.settling[point] : carried.arriving[point]) += yearly;
    }
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
        if (in_sea(rivers, point)) {
            continue;
        }
        const double thickness =
            sediment.settling[point] * years / (sphere.area(point) * body.radius * body.radius);
        sediment.alluvium[point] = thickness;
        sediment.lake_depth[point] = std::max(0.0, lakes.depth[point] - thickness);
    }
    spread_deltas(sphere, rivers, years, body.radius, sediment);
    return sediment;
}

} // namespace hog::formation
