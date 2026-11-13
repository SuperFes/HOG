#pragma once

#include <hog/core/math.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

namespace hog::globe {

// The way between two directions from the planet's centre a river may take: across a corridor
// `half_width` radians either side of the straight line, in `steps` along it and `lanes` either
// side of it.
struct Corridor {
    DVec3 from{0.0};
    DVec3 to{0.0};
    double half_width = 0.0;
    int steps = 24;
    int lanes = 6;
};

namespace detail {

// Cuts each corner a quarter of the way along both its sides (Chaikin), keeping the ends.
inline std::vector<DVec3> cut_corners(const std::vector<DVec3>& line) {
    if (line.size() < 3) {
        return line;
    }
    std::vector<DVec3> cut{line.front()};
    for (std::size_t k = 0; k + 1 < line.size(); ++k) {
        const DVec3& a = line[k];
        const DVec3& b = line[k + 1];
        if (k > 0) {
            cut.push_back(glm::normalize((0.75 * a) + (0.25 * b)));
        }
        if (k + 2 < line.size()) {
            cut.push_back(glm::normalize((0.25 * a) + (0.75 * b)));
        }
    }
    cut.push_back(line.back());
    return cut;
}

} // namespace detail

// The course water cuts from `corridor.from` to `corridor.to`: of the ways across the corridor,
// moving at most a lane aside each step, the one taking the least effort to cut, `effort(at)`
// a metre at each direction `at` the lanes cross the steps; its corners then worn round, as water
// wears them. Directions, from end to end.
template <class Effort>
[[nodiscard]] std::vector<DVec3> course(const Corridor& corridor, const Effort& effort) {
    const int lanes = (2 * corridor.lanes) + 1;
    const DVec3 side = glm::normalize(glm::cross(corridor.from, corridor.to));
    const auto at = [&](int step, int lane) {
        const double t = static_cast<double>(step) / corridor.steps;
        const DVec3 line = glm::normalize(corridor.from + ((corridor.to - corridor.from) * t));
        const double aside =
            corridor.half_width * static_cast<double>(lane - corridor.lanes) / corridor.lanes;
        return glm::normalize(line + (side * aside));
    };
    const auto index = [lanes](int step, int lane) {
        return (static_cast<std::size_t>(step) * static_cast<std::size_t>(lanes)) +
               static_cast<std::size_t>(lane);
    };
    const auto size =
        static_cast<std::size_t>(corridor.steps + 1) * static_cast<std::size_t>(lanes);
    std::vector<DVec3> place(size);
    std::vector<double> hard(size);
    for (int step = 0; step <= corridor.steps; ++step) {
        for (int lane = 0; lane < lanes; ++lane) {
            place[index(step, lane)] = at(step, lane);
            hard[index(step, lane)] = effort(place[index(step, lane)]);
        }
    }
    constexpr double never = std::numeric_limits<double>::infinity();
    std::vector<double> least(size, never);
    std::vector<int> came(size, corridor.lanes);
    least[index(0, corridor.lanes)] = 0.0;
    for (int step = 0; step < corridor.steps; ++step) {
        for (int lane = 0; lane < lanes; ++lane) {
            const double so_far = least[index(step, lane)];
            if (so_far == never) {
                continue;
            }
            const DVec3& here = place[index(step, lane)];
            for (int next = std::max(0, lane - 1); next <= std::min(lanes - 1, lane + 1); ++next) {
                const DVec3& there = place[index(step + 1, next)];
                const double cost =
                    so_far + (0.5 * (hard[index(step, lane)] + hard[index(step + 1, next)]) *
                              glm::length(there - here));
                if (cost < least[index(step + 1, next)]) {
                    least[index(step + 1, next)] = cost;
                    came[index(step + 1, next)] = lane;
                }
            }
        }
    }
    std::vector<DVec3> line(static_cast<std::size_t>(corridor.steps) + 1);
    int lane = corridor.lanes;
    for (int step = corridor.steps; step >= 0; --step) {
        line[static_cast<std::size_t>(step)] = place[index(step, lane)];
        lane = came[index(step, lane)];
    }
    line.front() = corridor.from;
    line.back() = corridor.to;
    return detail::cut_corners(line);
}

} // namespace hog::globe
