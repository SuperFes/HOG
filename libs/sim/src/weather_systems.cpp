#include "weather_stages.hpp"

#include <hog/core/hash.hpp>
#include <hog/planet/projection.hpp>
#include <hog/planet/tangent_frame.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace hog::sim {

double StormSystem::strength(double growing) const {
    const double through = std::clamp(static_cast<double>(age / life), 0.0, 1.0);
    const double shape = through < growing ? through / growing : (1.0 - through) / (1.0 - growing);
    // Smoothstep, so systems ease in and out.
    return static_cast<double>(depth) * shape * shape * (3.0 - (2.0 * shape));
}

namespace detail {

namespace {

constexpr int candidates = 24;

// Uniform in [0, 1), the same for the same inputs on every platform.
template <typename... Values>
double random(std::uint64_t seed, Values... values) {
    constexpr double scale = 1.0 / static_cast<double>(std::uint64_t{1} << 53U);
    return static_cast<double>(hash_values(seed, values...) >> 11U) * scale;
}

double lerp(double low, double high, double t) {
    return low + ((high - low) * t);
}

// How strongly the jet shears over the surface wind: where the upper air's temperature changes
// fastest, which is where Earth's lows form.
double baroclinicity(const WeatherFields& fields, std::uint32_t index) {
    return static_cast<double>(glm::length(fields.jet[index] - fields.wind[index]));
}

void travel(const StepContext& step, const WeatherFields& fields, StormSystem& system) {
    const SystemTuning& tuning = step.config->tuning.systems;
    const std::uint32_t cell = step.grid->index(planet::cell_at(system.centre, step.grid->level()));
    const planet::TangentFrame frame = planet::tangent_frame(system.centre);
    const double poleward = system.centre.z < 0.0 ? -tuning.poleward : tuning.poleward;
    const DVec2 velocity = (DVec2{fields.jet[cell]} * tuning.steering) + DVec2{0.0, poleward};
    const double speed = glm::length(velocity);
    if (speed > 0.0) {
        const double angle = speed * step.seconds / earth_radius;
        const DVec3 heading = ((frame.east * velocity.x) + (frame.north * velocity.y)) / speed;
        system.centre =
            glm::normalize((system.centre * std::cos(angle)) + (heading * std::sin(angle)));
    }
    system.age += static_cast<float>(step.seconds);
}

void spawn(const StepContext& step, const WeatherFields& fields, std::vector<StormSystem>& systems,
           std::uint32_t& next_id, std::int64_t step_number, int hemisphere, int which) {
    const SystemTuning& tuning = step.config->tuning.systems;
    const std::uint64_t seed = step.config->seed;
    std::uint32_t best = 0;
    double strongest = -1.0;
    for (int candidate = 0; candidate < candidates; ++candidate) {
        const auto index =
            static_cast<std::uint32_t>(random(seed, step_number, hemisphere, which, candidate) *
                                       static_cast<double>(step.grid->size()));
        const double sin_latitude = step.grid->frame(index).up.z;
        const double latitude = std::asin(sin_latitude) / degree;
        const bool here = hemisphere == 0 ? latitude > 0.0 : latitude < 0.0;
        if (!here || std::abs(latitude) < tuning.min_latitude ||
            std::abs(latitude) > tuning.max_latitude) {
            continue;
        }
        const DVec3 up = step.grid->frame(index).up;
        const bool crowded = std::ranges::any_of(systems, [&](const StormSystem& other) {
            const double apart = std::acos(std::clamp(glm::dot(up, other.centre), -1.0, 1.0));
            return apart < tuning.spacing * tuning.max_radius * degree;
        });
        if (crowded) {
            continue;
        }
        if (const double shear = baroclinicity(fields, index); shear > strongest) {
            strongest = shear;
            best = index;
        }
    }
    if (strongest < 0.0) {
        return;
    }
    const auto draw = [&](int what) { return random(seed, step_number, hemisphere, which, what); };
    constexpr int depth = 100;
    constexpr int radius = 101;
    constexpr int life = 102;
    systems.push_back(StormSystem{
        .id = next_id++,
        .centre = step.grid->frame(best).up,
        .depth = static_cast<float>(lerp(tuning.min_depth, tuning.max_depth, draw(depth))),
        .radius =
            static_cast<float>(lerp(tuning.min_radius, tuning.max_radius, draw(radius)) * degree),
        .age = 0.0F,
        .life =
            static_cast<float>(lerp(tuning.min_life.count(), tuning.max_life.count(), draw(life))),
    });
}

} // namespace

void update_systems(const StepContext& step, const WeatherFields& fields,
                    std::vector<StormSystem>& systems, std::uint32_t& next_id,
                    std::int64_t step_number) {
    const SystemTuning& tuning = step.config->tuning.systems;
    for (StormSystem& system : systems) {
        travel(step, fields, system);
    }
    std::erase_if(systems, [](const StormSystem& system) { return system.age >= system.life; });

    const double expected = tuning.per_day * step.seconds / 86'400.0;
    for (int hemisphere = 0; hemisphere < 2; ++hemisphere) {
        // Whole expected systems always form; the fraction left over forms by chance.
        const double whole = std::floor(expected);
        const int count =
            static_cast<int>(whole) +
            (random(step.config->seed, step_number, hemisphere) < expected - whole ? 1 : 0);
        for (int which = 0; which < count && systems.size() < tuning.limit; ++which) {
            spawn(step, fields, systems, next_id, step_number, hemisphere, which);
        }
    }
}

void fronts(const StepContext& step, WeatherFields& fields, std::uint32_t index) {
    const FrontTuning& tuning = step.config->tuning.fronts;
    const double growing = step.config->tuning.systems.growing;
    const DVec3 up = step.grid->frame(index).up;
    const DVec2 warm_end{tuning.warm_east, tuning.warm_poleward};
    const DVec2 cold_end{tuning.cold_east, tuning.cold_poleward};
    const double reach =
        std::max(glm::length(warm_end), glm::length(cold_end)) + (2.0 * tuning.width);
    double front = 0.0;
    for (const StormSystem& system : step.systems) {
        const auto radius = static_cast<double>(system.radius);
        if (glm::dot(up, system.centre) < std::cos(std::min(reach * radius, 3.0))) {
            continue;
        }
        const planet::TangentFrame frame = planet::tangent_frame(system.centre);
        // Where the cell lies from the centre in radii, poleward up in either hemisphere.
        DVec2 at = DVec2{glm::dot(up, frame.east), glm::dot(up, frame.north)} / radius;
        if (system.centre.z < 0.0) {
            at.y = -at.y;
        }
        const double share =
            system.strength(growing) / std::max(static_cast<double>(system.depth), 1e-9);
        const auto along = [&](DVec2 end) {
            const double t = std::clamp(glm::dot(at, end) / glm::dot(end, end), 0.0, 1.0);
            const double away = glm::length(at - (end * t)) / tuning.width;
            return share * std::exp(-away * away);
        };
        const double warm = along(warm_end);
        const double cold = along(cold_end);
        const double strongest = warm >= cold ? warm : -cold;
        if (std::abs(strongest) > std::abs(front)) {
            front = strongest;
        }
    }
    fields.front[index] = static_cast<float>(front);
}

} // namespace detail

} // namespace hog::sim
