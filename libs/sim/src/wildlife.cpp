#include "wildlife_stages.hpp"

#include <hog/core/assert.hpp>
#include <hog/core/hash.hpp>
#include <hog/sim/calendar.hpp>
#include <hog/sim/weather.hpp>

#include <numbers>
#include <utility>

namespace hog::sim {

namespace {

GameDuration positive(GameDuration duration) {
    return duration > GameDuration::zero() ? duration : GameDuration{1};
}

} // namespace

std::vector<Habitat> habitat_for(const CellGrid& grid, const planet::World& world) {
    std::vector<Habitat> habitat(grid.size());
    for (std::uint32_t cell = 0; cell < grid.size(); ++cell) {
        const planet::Biome biome = world.state(grid.cell(cell)).biome;
        habitat[cell] = Habitat{.biome = biome, .ocean = biome == planet::Biome::Ocean};
    }
    return habitat;
}

Conditions conditions_of(const Weather& weather) {
    return Conditions{.temperature = weather.fields().recent_temperature,
                      .precipitation = weather.fields().recent_precipitation};
}

Wildlife::Wildlife(WildlifeConfig config, std::vector<Habitat> habitat, GameTime now)
    : config_(std::move(config)), grid_(config_.level), habitat_(std::move(habitat)),
      time_(detail::last_step(now, positive(config_.step))) {
    HOG_ASSERT(config_.step > GameDuration::zero(), "wildlife step must be positive");
    HOG_ASSERT(habitat_.size() == grid_.size(), "need habitat for every cell");
    HOG_ASSERT(config_.planet_radius > 0.0, "planet radius must be positive");
    const std::uint32_t size = grid_.size();
    const double square_km = config_.planet_radius * config_.planet_radius / 1e6;
    areas_.reserve(size);
    back_.resize(size);
    for (std::uint32_t cell = 0; cell < size; ++cell) {
        areas_.push_back(grid_.area(cell) * square_km);
        for (std::size_t k = 0; k < 4; ++k) {
            const auto& theirs = grid_.neighbors(grid_.neighbors(cell)[k]);
            back_[cell][k] =
                static_cast<std::uint8_t>(std::ranges::find(theirs, cell) - theirs.begin());
        }
    }

    const auto& species = config_.ecology.species;
    fields_.forage.resize(size);
    fields_.carrion.resize(size);
    for (std::uint32_t cell = 0; cell < size; ++cell) {
        fields_.forage[cell] = static_cast<float>(
            config_.ecology.forage.capacity.at(static_cast<std::size_t>(habitat_[cell].biome)));
    }
    for (const Species& one : species) {
        Population& population = fields_.populations.emplace_back();
        population.count.resize(size);
        population.fed.assign(size, 1.0F);
        for (std::uint32_t cell = 0; cell < size; ++cell) {
            const Habitat& here = habitat_[cell];
            population.count[cell] =
                detail::passable(one, here)
                    ? static_cast<float>(one.density *
                                         one.habitat.at(static_cast<std::size_t>(here.biome)) *
                                         areas_[cell])
                    : 0.0F;
        }
        // Each region's boom peaks in its own year, the same for a species whatever its order.
        std::vector<float>& peaks = peaks_.emplace_back();
        if (one.cycle.years > 0.0) {
            const int level = std::min(one.cycle.region_level, grid_.level());
            const std::uint64_t name = hash_string(one.name);
            peaks.resize(size);
            for (std::uint32_t cell = 0; cell < size; ++cell) {
                const std::uint64_t region = grid_.cell(cell).parent(level).raw();
                peaks[cell] = static_cast<float>(
                    detail::fraction(hash_values(config_.seed, name, region)) * one.cycle.years);
            }
        }
    }
    const int region_level = std::clamp(config_.region_level, 0, grid_.level());
    const std::uint32_t side = 1U << static_cast<unsigned>(region_level);
    region_count_ = 6 * side * side;
    regions_.reserve(size);
    for (std::uint32_t cell = 0; cell < size; ++cell) {
        const planet::FaceIJ at = grid_.cell(cell).parent(region_level).to_face_ij();
        regions_.push_back((static_cast<std::uint32_t>(at.face) * side * side) + (at.j * side) +
                           at.i);
    }
    for (const Species& one : species) {
        std::vector<bool>& livable = livable_.emplace_back(region_count_);
        for (std::uint32_t cell = 0; cell < size; ++cell) {
            if (detail::passable(one, habitat_[cell]) &&
                one.habitat.at(static_cast<std::size_t>(habitat_[cell].biome)) > 0.0) {
                livable[regions_[cell]] = true;
            }
        }
    }
    const auto held_now = held(fields_);
    for (std::uint32_t kind = 0; kind < species.size(); ++kind) {
        presence_.push_back(present(kind, held_now[kind], time_));
    }

    appeal_.resize(size);
    scent_.resize(size);
    scent_next_.resize(size);
    leaving_.resize(size);
    arriving_.resize(size);
}

std::uint64_t Wildlife::advance_to(GameTime now, const Conditions& conditions,
                                   jobs::JobSystem& jobs) {
    HOG_ASSERT(conditions.temperature.size() == grid_.size() &&
                   conditions.precipitation.size() == grid_.size(),
               "need conditions for every cell");
    std::uint64_t steps = 0;
    while (next_step() <= now) {
        step(next_step(), conditions, jobs);
        ++steps;
    }
    return steps;
}

void Wildlife::step(GameTime at, const Conditions& conditions, jobs::JobSystem& jobs) {
    const GameTime middle = at - (config_.step / 2);
    const double seconds_per_year = days_per_year * 86'400.0;
    const detail::LifeContext context{
        .config = &config_,
        .grid = &grid_,
        .habitat = habitat_,
        .areas = areas_,
        .peaks = peaks_,
        .conditions = conditions,
        .days = Seconds{config_.step}.count() / 86'400.0,
        .season_day = Calendar::solar_longitude(middle) / (2.0 * std::numbers::pi) * days_per_year,
        .year = Seconds{middle.time_since_epoch()}.count() / seconds_per_year,
    };
    detail::for_cells(&jobs, grid_.size(), [&](std::uint32_t cell) {
        detail::grow(context, fields_, cell);
        detail::feed(context, fields_, cell);
        detail::live(context, fields_, cell);
    });
    for (std::uint32_t species = 0; species < config_.ecology.species.size(); ++species) {
        move(context, species, jobs);
    }
    recover(context, at);
    time_ = at;
}

void Wildlife::move(const detail::LifeContext& context, std::uint32_t species,
                    jobs::JobSystem& jobs) {
    const Species& one = config_.ecology.species[species];
    std::vector<float>& count = fields_.populations[species].count;
    const auto open = [&](std::uint32_t cell) { return detail::passable(one, habitat_[cell]); };

    detail::for_cells(&jobs, grid_.size(), [&](std::uint32_t cell) {
        appeal_[cell] = static_cast<float>(detail::appeal(context, fields_, species, cell));
        scent_[cell] = appeal_[cell];
    });
    // Better ground far off draws animals less the further it is.
    const auto fade = static_cast<float>(config_.scent_fade);
    for (int reach = 0; reach < config_.scent_reach; ++reach) {
        detail::for_cells(&jobs, grid_.size(), [&](std::uint32_t cell) {
            float best = 0.0F;
            for (const std::uint32_t next : grid_.neighbors(cell)) {
                best = std::max(best, scent_[next]);
            }
            scent_next_[cell] = open(cell) ? std::max(appeal_[cell], fade * best) : 0.0F;
        });
        std::swap(scent_, scent_next_);
    }

    detail::for_cells(&jobs, grid_.size(), [&](std::uint32_t cell) {
        leaving_[cell] = detail::leaving(context, appeal_, scent_, species, cell);
    });

    // Each cell gathers its arrivals, so no two cells write to the same place.
    detail::for_cells(&jobs, grid_.size(), [&](std::uint32_t cell) {
        const std::array<float, 4>& leaving = leaving_[cell];
        double here =
            static_cast<double>(count[cell]) *
            (1.0 - static_cast<double>(leaving[0] + leaving[1] + leaving[2] + leaving[3]));
        const auto& next = grid_.neighbors(cell);
        for (std::size_t k = 0; k < 4; ++k) {
            here += static_cast<double>(count[next[k]]) *
                    static_cast<double>(leaving_[next[k]][back_[cell][k]]);
        }
        arriving_[cell] = static_cast<float>(here);
    });
    detail::for_cells(&jobs, grid_.size(), [&](std::uint32_t cell) {
        float& animals = count[cell];
        animals = arriving_[cell];
        if (static_cast<double>(animals) < config_.extinct_below) {
            fields_.carrion[cell] +=
                static_cast<float>(static_cast<double>(animals) * one.mass / areas_[cell]);
            animals = 0.0F;
        }
    });
}

double Wildlife::total(std::uint32_t species) const {
    double sum = 0.0;
    for (const float count : fields_.populations[species].count) {
        sum += static_cast<double>(count);
    }
    return sum;
}

float Wildlife::take(std::uint32_t species, std::uint32_t cell, float count) {
    float& here = fields_.populations[species].count[cell];
    const float taken = std::clamp(count, 0.0F, here);
    here -= taken;
    return taken;
}

void Wildlife::add(std::uint32_t species, std::uint32_t cell, float count) {
    HOG_ASSERT(count >= 0.0F, "can't add fewer than no animals");
    fields_.populations[species].count[cell] += count;
}

} // namespace hog::sim
