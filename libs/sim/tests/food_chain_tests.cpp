#include <hog/jobs/job_system.hpp>
#include <hog/planet/world.hpp>
#include <hog/sim/ecology.hpp>
#include <hog/sim/weather.hpp>
#include <hog/sim/wildlife.hpp>
#include <hog/testing/quick.hpp>
#include <hog/testing/worlds.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <numbers>
#include <span>
#include <string_view>
#include <vector>

using namespace std::chrono;
using hog::GameTime;
using hog::jobs::JobSystem;
using hog::sim::Conditions;
using hog::sim::Wildlife;

namespace {

constexpr int level = 3;
constexpr int run_years = 20;
constexpr int reach_years = 5;
constexpr GameTime march_equinox{(sys_days{2026y / March / 20} + hours{15}).time_since_epoch()};

// A year of the planet's weather as wildlife sees it, one day at a time, to replay year after
// year: far cheaper than running the weather for every year.
struct RecordedYear {
    std::vector<std::vector<float>> temperature;
    std::vector<std::vector<float>> precipitation;

    RecordedYear(const hog::planet::World& world, JobSystem& jobs) {
        const hog::sim::WeatherConfig config{.level = level,
                                             .step = hog::testing::full_or(hours{2}, hours{6}),
                                             .planet_radius = 115'000.0,
                                             .seed = 1};
        hog::sim::Weather weather{config, hog::sim::ground_for(hog::sim::CellGrid{level}, world),
                                  march_equinox};
        for (int day = 1; day <= 365; ++day) {
            weather.advance_to(march_equinox + days{day}, jobs);
            temperature.push_back(weather.fields().recent_temperature);
            precipitation.push_back(weather.fields().recent_precipitation);
        }
    }

    [[nodiscard]] Conditions on(int day) const {
        return Conditions{.temperature = temperature[static_cast<std::size_t>(day)],
                          .precipitation = precipitation[static_cast<std::size_t>(day)]};
    }
};

// The lag, in years, at which `follower` best tracks `leader`.
int lag(std::span<const double> leader, std::span<const double> follower) {
    const auto correlation = [&](std::size_t shift) {
        const std::size_t count = leader.size() - shift;
        double mean_a = 0.0;
        double mean_b = 0.0;
        for (std::size_t i = 0; i < count; ++i) {
            mean_a += leader[i];
            mean_b += follower[i + shift];
        }
        mean_a /= static_cast<double>(count);
        mean_b /= static_cast<double>(count);
        double ab = 0.0;
        double aa = 0.0;
        double bb = 0.0;
        for (std::size_t i = 0; i < count; ++i) {
            ab += (leader[i] - mean_a) * (follower[i + shift] - mean_b);
            aa += (leader[i] - mean_a) * (leader[i] - mean_a);
            bb += (follower[i + shift] - mean_b) * (follower[i + shift] - mean_b);
        }
        return ab / std::sqrt(aa * bb);
    };
    std::size_t best = 0;
    for (std::size_t shift = 1; shift <= leader.size() / 2; ++shift) {
        if (correlation(shift) > correlation(best)) {
            best = shift;
        }
    }
    return static_cast<int>(best);
}

struct Ids {
    std::uint32_t deer = 0;
    std::uint32_t wolf = 0;
    std::uint32_t raven = 0;
};

std::uint32_t id_of(const hog::sim::Ecology& ecology, std::string_view name) {
    const auto found = ecology.find(name);
    REQUIRE(found);
    return found.value_or(0);
}

// Yearly totals of each species, how far north the deer reach in the last summers and late
// winters, and what was reported along the way.
struct Run {
    std::vector<std::vector<double>> totals;
    double summer_reach = 0.0;
    double winter_reach = 0.0;
    std::vector<hog::sim::EcologyEvent> events;
};

// Deer north of 35° as a share of all northern deer.
double northern_reach(const Wildlife& wildlife, const hog::sim::CellGrid& grid,
                      std::uint32_t deer) {
    double north = 0.0;
    double far = 0.0;
    for (std::uint32_t cell = 0; cell < grid.size(); ++cell) {
        const double z = grid.frame(cell).up.z;
        const auto count = static_cast<double>(wildlife.fields().populations[deer].count[cell]);
        north += z > 0.0 ? count : 0.0;
        far += z > std::sin(35.0 * std::numbers::pi / 180.0) ? count : 0.0;
    }
    return far / north;
}

Run run(Wildlife& wildlife, const RecordedYear& weather, const hog::sim::CellGrid& grid,
        std::uint32_t deer, JobSystem& jobs) {
    Run result{.totals = std::vector<std::vector<double>>(wildlife.config().ecology.species.size()),
               .events = {}};
    for (int year = 0; year < run_years; ++year) {
        for (int day = 0; day < 365; ++day) {
            wildlife.advance_to(march_equinox + days{(year * 365) + day + 1}, weather.on(day),
                                jobs);
            // Midsummer, and late winter once the cold has thinned them, over the last years.
            if (year >= run_years - reach_years && day == 95) {
                result.summer_reach += northern_reach(wildlife, grid, deer) / reach_years;
            }
            if (year >= run_years - reach_years && day == 340) {
                result.winter_reach += northern_reach(wildlife, grid, deer) / reach_years;
            }
        }
        for (std::uint32_t species = 0; species < result.totals.size(); ++species) {
            result.totals[species].push_back(wildlife.total(species));
        }
        std::ranges::move(wildlife.drain_events(), std::back_inserter(result.events));
    }
    return result;
}

// Deer if they filled every cell to crowding.
double deer_room(const Wildlife& wildlife, std::span<const hog::sim::Habitat> habitat,
                 const hog::sim::Species& deer) {
    double room = 0.0;
    for (std::uint32_t cell = 0; cell < habitat.size(); ++cell) {
        if (!habitat[cell].ocean) {
            room += deer.crowded * deer.habitat.at(static_cast<std::size_t>(habitat[cell].biome)) *
                    wildlife.area(cell);
        }
    }
    return room;
}

// Land animals found at sea, and the share of deer where they thrive.
struct Placement {
    int land_at_sea = 0;
    double deer_at_home = 0.0;
};

Placement placement(const Wildlife& wildlife, std::span<const hog::sim::Habitat> habitat,
                    std::uint32_t deer) {
    const auto& species = wildlife.config().ecology.species;
    Placement result;
    double deer_count = 0.0;
    double deer_at_home = 0.0;
    for (std::uint32_t cell = 0; cell < habitat.size(); ++cell) {
        for (std::uint32_t kind = 0; kind < species.size(); ++kind) {
            const bool stranded = habitat[cell].ocean &&
                                  species[kind].moves == hog::sim::Moves::Land &&
                                  wildlife.fields().populations[kind].count[cell] > 0.0F;
            result.land_at_sea += stranded ? 1 : 0;
        }
        const auto count = static_cast<double>(wildlife.fields().populations[deer].count[cell]);
        const auto biome = static_cast<std::size_t>(habitat[cell].biome);
        deer_count += count;
        deer_at_home += species[deer].habitat.at(biome) >= 0.5 ? count : 0.0;
    }
    result.deer_at_home = deer_at_home / deer_count;
    return result;
}

} // namespace

TEST_CASE("the development species make a food chain", "[sim][wildlife][food-chain]") {
    auto ecology = hog::sim::load_ecology(HOG_SOURCE_DIR "/config/ecology.toml");
    REQUIRE(ecology);
    const Ids ids{.deer = id_of(*ecology, "deer"),
                  .wolf = id_of(*ecology, "wolf"),
                  .raven = id_of(*ecology, "raven")};

    const hog::planet::World world{hog::planet::PlanetConfig{
        .seed = 1, .radius = 115'000.0, .erosion_cell = hog::testing::coarse_erosion_cell}};
    JobSystem jobs{0};
    const RecordedYear weather{world, jobs};
    const hog::sim::CellGrid grid{level};
    const auto habitat = hog::sim::habitat_for(grid, world);
    hog::sim::WildlifeConfig config{.level = level, .seed = 1};
    config.ecology = *ecology;
    Wildlife wildlife{config, habitat, march_equinox};
    const Run years = run(wildlife, weather, grid, ids.deer, jobs);

    // Every species lives on, never crashing to a remnant.
    for (const auto& yearly : years.totals) {
        const auto [low, high] = std::ranges::minmax(yearly);
        CHECK(low > 0.05 * high);
    }

    // Deer rise and fall over years, but never fill the land.
    const auto [fewest, most] = std::ranges::minmax(years.totals[ids.deer]);
    CHECK(most < deer_room(wildlife, habitat, ecology->species[ids.deer]));
    CHECK(most > 2.0 * fewest);

    // Wolves follow the deer, and ravens, which live largely on the dead, follow both.
    const int wolves_behind = lag(years.totals[ids.deer], years.totals[ids.wolf]);
    CHECK(wolves_behind >= 1);
    CHECK(wolves_behind <= 8);
    CHECK(lag(years.totals[ids.deer], years.totals[ids.raven]) >= 1);

    // Winter thins the deer out of the north.
    CHECK(years.winter_reach < years.summer_reach);

    // Natural swings are reported now and then, not every few days, and with no one hunting, no
    // hunters are named.
    const auto strays = std::ranges::count_if(years.events, [](const auto& event) {
        using Kind = hog::sim::EcologyEvent::Kind;
        return event.kind == Kind::Collapse || event.kind == Kind::Overpopulation;
    });
    CHECK(strays < run_years * 8);
    CHECK(
        std::ranges::all_of(years.events, [](const auto& event) { return event.hunters.empty(); }));

    // Each lives where it can: land animals on land, and deer mostly where they thrive.
    const Placement placed = placement(wildlife, habitat, ids.deer);
    CHECK(placed.land_at_sea == 0);
    CHECK(placed.deer_at_home > 0.8);
}
