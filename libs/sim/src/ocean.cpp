#include "weather_stages.hpp"

#include <hog/core/assert.hpp>
#include <hog/sim/ocean.hpp>
#include <hog/sim/weather.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

namespace hog::sim {

namespace {

using detail::earth_radius;

constexpr double air_density = 1.2;       // kg/m³
constexpr double water_density = 1'025.0; // kg/m³

DVec3 world(const planet::TangentFrame& frame, Vec2 vector) {
    return (frame.east * static_cast<double>(vector.x)) +
           (frame.north * static_cast<double>(vector.y));
}

DVec2 local(const planet::TangentFrame& frame, DVec3 vector) {
    return {glm::dot(vector, frame.east), glm::dot(vector, frame.north)};
}

Vec2 capped(DVec2 vector, double limit) {
    const double length = glm::length(vector);
    return Vec2{length > limit ? vector * (limit / length) : vector};
}

Vec2 wind_stress(const OceanTuning& tuning, Vec2 wind) {
    const DVec2 air{wind};
    return Vec2{air * (air_density * tuning.drag * glm::length(air))};
}

} // namespace

OceanFields::OceanFields(std::size_t size)
    : temperature(size), current(size, Vec2{0.0F}), stress(size, Vec2{0.0F}), stream(size) {}

Ocean::Ocean(const CellGrid& grid, std::span<const Ground> ground, const OceanTuning& tuning,
             double rotation, std::span<const Vec2> wind, double sin_declination)
    : tuning_(tuning), rotation_(rotation), fields_(grid.size()), forcing_(grid.size()),
      next_(grid.size()), upwelling_(grid.size()), carried_(grid.size()) {
    HOG_ASSERT(ground.size() == grid.size(), "need ground for every cell");
    HOG_ASSERT(wind.size() == grid.size(), "need wind for every cell");
    wet_.reserve(grid.size());
    mean_.reserve(grid.size());
    spin_weights_.reserve(grid.size());
    for (std::uint32_t index = 0; index < grid.size(); ++index) {
        wet_.push_back(ground[index].ocean);
        mean_.push_back(ground[index].mean_temperature);
        coast_ = coast_ || !ground[index].ocean;
        const double sin_latitude = grid.frame(index).up.z;
        const double spin_gradient = 2.0 * rotation *
                                     std::sqrt(std::max(1.0 - (sin_latitude * sin_latitude), 0.0)) /
                                     earth_radius;
        std::array<double, 4> weights{};
        for (std::size_t k = 0; k < weights.size(); ++k) {
            const DVec2 offset = grid.offsets(index)[k];
            const double length = glm::length(offset);
            weights[k] = spin_gradient * std::max(offset.x / length, 0.0) / (length * earth_radius);
        }
        spin_weights_.push_back(weights);
        fields_.stress[index] = wind_stress(tuning_, wind[index]);
        fields_.temperature[index] = static_cast<float>(target(grid, index, sin_declination));
    }
    settle(grid, tuning_.first_settling, nullptr);
    find_currents(grid, nullptr);
}

double Ocean::target(const CellGrid& grid, std::uint32_t index, double sin_declination) const {
    return static_cast<double>(mean_[index]) +
           (tuning_.seasons * sin_declination * grid.frame(index).up.z);
}

void Ocean::set_fields(const CellGrid& grid, OceanFields fields) {
    HOG_ASSERT(fields.size() == grid.size() && fields.current.size() == grid.size() &&
                   fields.stress.size() == grid.size() && fields.stream.size() == grid.size(),
               "need the sea for every cell");
    fields_ = std::move(fields);
}

void Ocean::step(const CellGrid& grid, std::span<const Vec2> wind, double sin_declination,
                 double seconds, jobs::JobSystem* jobs) {
    drive(grid, wind, seconds, jobs);
    warm(grid, sin_declination, seconds, jobs);
}

void Ocean::drive(const CellGrid& grid, std::span<const Vec2> wind, double seconds,
                  jobs::JobSystem* jobs) {
    HOG_ASSERT(wind.size() == grid.size(), "need wind for every cell");
    const auto remembering = static_cast<float>(detail::approach(seconds, tuning_.stress_memory));
    for (std::uint32_t index = 0; index < grid.size(); ++index) {
        Vec2& stress = fields_.stress[index];
        stress += (wind_stress(tuning_, wind[index]) - stress) * remembering;
    }
    settle(grid, tuning_.settling, jobs);
    find_currents(grid, jobs);
}

void Ocean::settle(const CellGrid& grid, int sweeps, jobs::JobSystem* jobs) {
    // The wind's twist: the curl of its stress, each neighbour's turned into this cell's frame.
    detail::for_cells(jobs, grid.size(), [&](std::uint32_t index) {
        const planet::TangentFrame& here = grid.frame(index);
        const DVec2 stress{fields_.stress[index]};
        DVec2 east_gradient{0.0};
        DVec2 north_gradient{0.0};
        for (std::size_t k = 0; k < 4; ++k) {
            const std::uint32_t there = grid.neighbors(index)[k];
            const DVec2 change =
                local(here, world(grid.frame(there), fields_.stress[there])) - stress;
            east_gradient += grid.gradient_weights(index)[k] * change.x;
            north_gradient += grid.gradient_weights(index)[k] * change.y;
        }
        const double curl = (north_gradient.x - east_gradient.y) / earth_radius;
        forcing_[index] = curl / (water_density * tuning_.layer);
    });

    // Friction * laplacian(stream) + spin gradient * d(stream)/d(east) = forcing, with the stream
    // still on land, so water runs along coasts and never through them.
    const double friction = tuning_.friction / (earth_radius * earth_radius);
    std::vector<double>& stream = fields_.stream;
    for (int sweep = 0; sweep < sweeps; ++sweep) {
        detail::for_cells(jobs, grid.size(), [&](std::uint32_t index) {
            if (!wet_[index]) {
                next_[index] = 0.0;
                return;
            }
            double around = 0.0;
            double total = 0.0;
            for (std::size_t k = 0; k < 4; ++k) {
                const double weight =
                    (friction * grid.laplacian_weights(index)[k]) + spin_weights_[index][k];
                around += weight * stream[grid.neighbors(index)[k]];
                total += weight;
            }
            next_[index] = (around - forcing_[index]) / total;
        });
        if (!coast_) {
            // Without land the stream is only known up to a constant; hold its mean at zero.
            double sum = 0.0;
            double area = 0.0;
            for (std::uint32_t index = 0; index < grid.size(); ++index) {
                sum += next_[index] * grid.area(index);
                area += grid.area(index);
            }
            for (double& value : next_) {
                value -= sum / area;
            }
        }
        double change = 0.0;
        double largest = 0.0;
        for (std::uint32_t index = 0; index < grid.size(); ++index) {
            change = std::max(change, std::abs(next_[index] - stream[index]));
            largest = std::max(largest, std::abs(next_[index]));
        }
        stream.swap(next_);
        if (change <= 1e-7 * largest) {
            break;
        }
    }
}

void Ocean::find_currents(const CellGrid& grid, jobs::JobSystem* jobs) {
    detail::for_cells(jobs, grid.size(), [&](std::uint32_t index) {
        if (!wet_[index]) {
            fields_.current[index] = Vec2{0.0F};
            return;
        }
        DVec2 gradient{0.0};
        for (std::size_t k = 0; k < 4; ++k) {
            gradient += grid.gradient_weights(index)[k] *
                        (fields_.stream[grid.neighbors(index)[k]] - fields_.stream[index]);
        }
        gradient /= earth_radius;
        fields_.current[index] = capped(DVec2{-gradient.y, gradient.x}, tuning_.max_current);
    });
}

void Ocean::find_upwelling(const CellGrid& grid, jobs::JobSystem* jobs) {
    const double floor = 2.0 * rotation_ * std::sin(tuning_.min_spin_latitude * detail::degree);
    // The water the wind drags along turns right of it in the north and left in the south.
    const auto pushed = [&](std::uint32_t index) {
        const double coriolis = 2.0 * rotation_ * grid.frame(index).up.z;
        const double held = std::abs(coriolis) < floor ? std::copysign(floor, coriolis) : coriolis;
        const Vec2 stress = fields_.stress[index];
        return DVec2{stress.y, -stress.x} / (water_density * held);
    };
    detail::for_cells(jobs, grid.size(), [&](std::uint32_t index) {
        if (!wet_[index]) {
            upwelling_[index] = 0.0;
            return;
        }
        const planet::TangentFrame& here = grid.frame(index);
        const DVec2 push = pushed(index);
        // What leaves across each edge, none of it into land.
        double outflow = 0.0;
        for (std::size_t k = 0; k < 4; ++k) {
            const std::uint32_t there = grid.neighbors(index)[k];
            if (wet_[there]) {
                const DVec2 theirs = local(here, world(grid.frame(there), Vec2{pushed(there)}));
                outflow += grid.laplacian_weights(index)[k] *
                           glm::dot(grid.offsets(index)[k], (push + theirs) * 0.5);
            }
        }
        upwelling_[index] = std::max(outflow / earth_radius, 0.0);
    });
}

void Ocean::warm(const CellGrid& grid, double sin_declination, double seconds,
                 jobs::JobSystem* jobs) {
    find_upwelling(grid, jobs);
    carried_ = fields_.temperature;
    const double settling = detail::approach(seconds, tuning_.response);
    detail::for_cells(jobs, grid.size(), [&](std::uint32_t index) {
        const double season = target(grid, index, sin_declination);
        if (!wet_[index]) {
            fields_.temperature[index] = static_cast<float>(season);
            return;
        }
        const DVec3 from = detail::departure(grid.frame(index), fields_.current[index], seconds);
        double warmth = grid.interpolate(from, [&](std::uint32_t cell) {
            return static_cast<double>(wet_[cell] ? carried_[cell] : carried_[index]);
        });
        warmth += (season - warmth) * settling;
        const double deep = season - tuning_.upwelled;
        const double lifted = 1.0 - std::exp(-upwelling_[index] * seconds / tuning_.layer);
        warmth += std::min(deep - warmth, 0.0) * lifted;
        fields_.temperature[index] = static_cast<float>(std::max(warmth, tuning_.freezing));
    });
}

} // namespace hog::sim
