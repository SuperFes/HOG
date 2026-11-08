#include <hog/formation/rivers.hpp>

#include <hog/core/portable_maths.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
#include <queue>
#include <ranges>
#include <span>
#include <utility>

namespace hog::formation {

namespace {

// Lengths and areas are reckoned at Earth's scale, as plate speeds are.
constexpr double earth_radius = 6'371'000.0;
constexpr int most_rounds = 200;

double angle(const DVec3& a, const DVec3& b) noexcept {
    return maths::atan2(glm::length(glm::cross(a, b)), glm::dot(a, b));
}

double smoothstep(double from, double to, double x) noexcept {
    const double t = std::clamp((x - from) / (to - from), 0.0, 1.0);
    return t * t * (3.0 - (2.0 * t));
}

// Land points ordered so each comes after the point its water goes to, from the sea inland.
std::vector<std::uint32_t> downstream_first(const Sphere& sphere,
                                            const std::vector<std::uint32_t>& receiver,
                                            const std::vector<std::uint8_t>& land) {
    std::vector<std::uint32_t> counts(sphere.size() + 1, 0);
    for (std::uint32_t point = 0; point < sphere.size(); ++point) {
        if (land[point] != 0) {
            ++counts[receiver[point] + 1];
        }
    }
    for (std::size_t n = 1; n < counts.size(); ++n) {
        counts[n] += counts[n - 1];
    }
    std::vector<std::uint32_t> upstream(counts.back());
    std::vector<std::uint32_t> filled(counts.begin(), counts.end() - 1);
    for (std::uint32_t point = 0; point < sphere.size(); ++point) {
        if (land[point] != 0) {
            upstream[filled[receiver[point]]++] = point;
        }
    }
    std::vector<std::uint32_t> order;
    order.reserve(sphere.size());
    std::vector<std::uint32_t> open;
    for (std::uint32_t point = 0; point < sphere.size(); ++point) {
        if (land[point] == 0) {
            open.push_back(point);
        }
    }
    for (std::size_t next = 0; next < open.size(); ++next) {
        const std::uint32_t point = open[next];
        for (std::uint32_t at = counts[point]; at < counts[point + 1]; ++at) {
            open.push_back(upstream[at]);
            order.push_back(upstream[at]);
        }
    }
    return order;
}

// What the land is made of for running water: where it is, and how readily it wears.
struct Ground {
    const Sphere* sphere;
    const Crust* crust;
    const Climate* climate;
    const Body* body;
    std::vector<std::uint8_t> land;
    std::vector<double> erodibility;
};

// First routing: flood inwards from the sea, lowest ground first, so every land point has a way
// down to the sea even across hollows in the crust.
void flood_from_sea(const Ground& ground, std::vector<std::uint32_t>& receiver) {
    const std::uint32_t size = ground.sphere->size();
    std::vector<std::uint8_t> reached(size, 0);
    using Entry = std::pair<double, std::uint32_t>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<>> open;
    for (std::uint32_t point = 0; point < size; ++point) {
        if (ground.land[point] == 0) {
            reached[point] = 1;
            open.emplace(0.0, point);
        }
    }
    while (!open.empty()) {
        const auto [level, point] = open.top();
        open.pop();
        for (const std::uint32_t next : ground.sphere->neighbors(point)) {
            if (reached[next] != 0) {
                continue;
            }
            reached[next] = 1;
            receiver[next] = point;
            open.emplace(std::max(level, ground.crust->base[next]), next);
        }
    }
}

// Water gathered down the network. What wears is gathered with a floor under the rain, as flash
// floods still cut deserts; the discharge kept is the water truly passing.
void gather(const Ground& ground, std::span<const std::uint32_t> order, Rivers& rivers,
            std::vector<double>& wearing) {
    const double cell_area = earth_radius * earth_radius;
    for (std::uint32_t point = 0; point < ground.sphere->size(); ++point) {
        const double gathering =
            ground.land[point] != 0 ? ground.sphere->area(point) * cell_area : 0.0;
        const double rain = ground.climate->rain[point];
        rivers.discharge[point] = std::max(rain, 0.0) * gathering;
        wearing[point] = std::max(rain, ground.body->driest_wear) * gathering;
    }
    for (const std::uint32_t point : std::views::reverse(order)) {
        rivers.discharge[rivers.receiver[point]] += rivers.discharge[point];
        wearing[rivers.receiver[point]] += wearing[point];
    }
}

// Up from the coast, each step as steep as balances wear against uplift there, a river having to
// pass a threshold before it cuts at all; no steeper than slopes stand before they slide, and
// never higher than the crust floats.
void raise(const Ground& ground, std::span<const std::uint32_t> order,
           std::span<const double> wearing, Rivers& rivers) {
    const Body& body = *ground.body;
    for (const std::uint32_t point : order) {
        const std::uint32_t down = rivers.receiver[point];
        const double below = ground.land[down] != 0 ? rivers.height[down] : 0.0;
        const double length =
            angle(ground.sphere->point(point), ground.sphere->point(down)) * earth_radius;
        const double flow = std::max(wearing[point], 1.0);
        const double slope = (ground.crust->uplift[point] + body.wear_threshold) /
                             (ground.erodibility[point] * maths::pow(flow, body.discharge_power));
        // Where the crust's cap falls under the ground downstream, the hollow is filled all but
        // flat, so water still finds its way out; lakes are left to a stage of their own.
        const double worn = below + (length * std::min(slope, body.steepest));
        rivers.height[point] =
            std::max(std::min(worn, ground.crust->base[point]), below + (1e-6 * length));
    }
}

// Water takes the steepest way down the land as it now stands; how many points changed course.
int reroute(const Ground& ground, std::span<const std::uint32_t> order, Rivers& rivers) {
    int changed = 0;
    for (const std::uint32_t point : order) {
        std::uint32_t lowest = rivers.receiver[point];
        double drop = 0.0;
        for (const std::uint32_t next : ground.sphere->neighbors(point)) {
            const double there = ground.land[next] != 0 ? rivers.height[next] : 0.0;
            const double length = angle(ground.sphere->point(point), ground.sphere->point(next));
            const double steepness = (rivers.height[point] - there) / length;
            if (steepness > drop) {
                drop = steepness;
                lowest = next;
            }
        }
        if (lowest != rivers.receiver[point]) {
            rivers.receiver[point] = lowest;
            ++changed;
        }
    }
    return changed;
}

} // namespace

Rivers rivers_on(const Sphere& sphere, const Crust& crust, const Climate& climate,
                 const Body& body) {
    const std::uint32_t size = sphere.size();
    Rivers rivers;
    rivers.height = crust.base;
    rivers.receiver.resize(size);
    rivers.discharge.assign(size, 0.0);
    rivers.wear.assign(size, 0.0);
    rivers.cover.assign(size, 0.0);

    Ground ground{.sphere = &sphere,
                  .crust = &crust,
                  .climate = &climate,
                  .body = &body,
                  .land = std::vector<std::uint8_t>(size),
                  .erodibility = std::vector<double>(size)};
    for (std::uint32_t point = 0; point < size; ++point) {
        ground.land[point] = crust.base[point] >= 0.0 ? 1 : 0;
        rivers.receiver[point] = point;
        // Plants take hold where it's wet and warm enough, and hold the ground against wear.
        rivers.cover[point] = smoothstep(0.15, 1.0, climate.rain[point]) *
                              smoothstep(-5.0, 5.0, climate.temperature[point]);
        ground.erodibility[point] =
            body.erodibility * (1.0 - (body.cover_holding * rivers.cover[point]));
    }
    flood_from_sea(ground, rivers.receiver);

    std::vector<double> wearing(size, 0.0);
    std::vector<std::uint32_t> order = downstream_first(sphere, rivers.receiver, ground.land);
    for (rivers.rounds = 1; rivers.rounds <= most_rounds; ++rivers.rounds) {
        gather(ground, order, rivers, wearing);
        raise(ground, order, wearing, rivers);
        if (reroute(ground, order, rivers) == 0) {
            break;
        }
        order = downstream_first(sphere, rivers.receiver, ground.land);
    }
    rivers.rounds = std::min(rivers.rounds, most_rounds);
    for (std::uint32_t point = 0; point < size; ++point) {
        rivers.wear[point] = ground.land[point] != 0 ? crust.uplift[point] : 0.0;
    }
    return rivers;
}

} // namespace hog::formation
