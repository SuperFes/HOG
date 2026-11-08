#include <hog/formation/fields.hpp>

#include <hog/core/hash.hpp>
#include <hog/core/portable_maths.hpp>

#include <functional>
#include <limits>
#include <queue>
#include <utility>

namespace hog::formation {

std::vector<double> sphere_noise(const Sphere& on, std::uint64_t seed, int coarsest, int finest) {
    std::vector<double> sum(on.size(), 0.0);
    double strength = 1.0;
    double total = 0.0;
    for (int level = coarsest; level <= finest; ++level) {
        const Sphere lattice{level, hash_values(seed, level, 1)};
        std::vector<double> values(lattice.size());
        for (std::uint32_t index = 0; index < lattice.size(); ++index) {
            values[index] =
                (static_cast<double>(hash_values(seed, level, index) >> 11U) * 0x1.0p-52) - 1.0;
        }
        std::uint32_t start = 0;
        for (std::uint32_t index = 0; index < on.size(); ++index) {
            const Sphere::Spot spot = lattice.locate(on.point(index), start);
            start = spot.corners[0];
            sum[index] += strength * lattice.sample(values, spot);
        }
        total += strength;
        strength *= 0.5;
    }
    for (double& value : sum) {
        value /= total;
    }
    return sum;
}

Nearest nearest_sources(const Sphere& sphere, std::span<const Source> sources) {
    Nearest nearest;
    nearest.source.assign(sphere.size(), Nearest::none);
    nearest.distance.assign(sphere.size(), std::numeric_limits<double>::infinity());
    using Entry = std::pair<double, std::uint32_t>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<>> open;
    const auto angle = [](const DVec3& a, const DVec3& b) {
        return maths::atan2(glm::length(glm::cross(a, b)), glm::dot(a, b));
    };
    const auto offer = [&](std::uint32_t point, std::uint32_t source) {
        const double distance = angle(sphere.point(point), sources[source].where);
        if (distance < nearest.distance[point]) {
            nearest.distance[point] = distance;
            nearest.source[point] = source;
            open.emplace(distance, point);
        }
    };
    for (std::uint32_t source = 0; source < sources.size(); ++source) {
        offer(sources[source].point, source);
    }
    while (!open.empty()) {
        const auto [distance, point] = open.top();
        open.pop();
        if (distance > nearest.distance[point]) {
            continue;
        }
        for (const std::uint32_t next : sphere.neighbors(point)) {
            offer(next, nearest.source[point]);
        }
    }
    return nearest;
}

} // namespace hog::formation
