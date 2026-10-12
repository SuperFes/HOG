#include <hog/jobs/job_system.hpp>
#include <hog/planet/world.hpp>
#include <hog/sim/weather.hpp>
#include <hog/sim/wildlife.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <set>
#include <vector>

using namespace std::chrono;
using hog::GameTime;
using hog::jobs::JobSystem;
using hog::planet::Biome;
using hog::sim::Conditions;
using hog::sim::Food;
using hog::sim::Habitat;
using hog::sim::Species;
using hog::sim::Wildlife;
using hog::sim::WildlifeConfig;

namespace {

constexpr int level = 3;
constexpr GameTime march_equinox{(sys_days{2026y / March / 20} + hours{15}).time_since_epoch()};

std::size_t at(Biome biome) {
    return static_cast<std::size_t>(biome);
}

Species grazer() {
    Species deer{
        .name = "deer",
        .mass = 80.0,
        .appetite = 3.0,
        .half_fed = 5'000.0,
        .diet = {Food{.kind = Food::Kind::Forage}},
        .density = 2.0,
        .crowded = 10.0,
        .births = 0.8,
        .lifespan = 10.0,
        .starving = 0.05,
    };
    deer.habitat[at(Biome::Grassland)] = 1.0;
    return deer;
}

WildlifeConfig config(std::vector<Species> species) {
    WildlifeConfig config{.level = level};
    config.ecology.forage.capacity[at(Biome::Grassland)] = 100'000.0;
    config.ecology.species = std::move(species);
    return config;
}

std::vector<Habitat> grassland() {
    return std::vector<Habitat>(hog::sim::CellGrid{level}.size(), Habitat{});
}

// Weather that stays the same everywhere.
struct Steady {
    std::vector<float> temperature;
    std::vector<float> precipitation;

    explicit Steady(float warmth = 15.0F, float wet = 3.0F)
        : temperature(hog::sim::CellGrid{level}.size(), warmth),
          precipitation(temperature.size(), wet) {}

    [[nodiscard]] Conditions conditions() const {
        return Conditions{.temperature = temperature, .precipitation = precipitation};
    }
};

double northern_share(const Wildlife& wildlife, std::uint32_t species) {
    double north = 0.0;
    const auto& count = wildlife.fields().populations[species].count;
    for (std::uint32_t cell = 0; cell < count.size(); ++cell) {
        if (wildlife.grid().frame(cell).up.z > 0.0) {
            north += static_cast<double>(count[cell]);
        }
    }
    return north / wildlife.total(species);
}

} // namespace

TEST_CASE("wildlife starts at its densities with forage grown", "[sim][wildlife]") {
    const Wildlife wildlife{config({grazer()}), grassland(), march_equinox + hours{30}};
    CHECK(wildlife.time() == march_equinox + hours{9});
    CHECK(wildlife.next_step() == wildlife.time() + days{1});
    double area = 0.0;
    for (std::uint32_t cell = 0; cell < wildlife.grid().size(); ++cell) {
        area += wildlife.area(cell);
    }
    // The planet's surface, 4πr², in km².
    CHECK(area > 166'000.0);
    CHECK(area < 166'300.0);
    CHECK(wildlife.total(0) > 1.99 * area);
    CHECK(wildlife.total(0) < 2.01 * area);
    CHECK(wildlife.fields().forage[0] == 100'000.0F);
    CHECK(wildlife.fields().populations[0].fed[0] == 1.0F);
}

TEST_CASE("a grazer settles where its forage and room allow", "[sim][wildlife]") {
    JobSystem jobs{1};
    Wildlife wildlife{config({grazer()}), grassland(), march_equinox};
    const Steady weather;
    std::vector<double> yearly;
    for (int year = 1; year <= 12; ++year) {
        wildlife.advance_to(march_equinox + days{365 * year}, weather.conditions(), jobs);
        yearly.push_back(wildlife.total(0));
    }
    double area = 0.0;
    for (std::uint32_t cell = 0; cell < wildlife.grid().size(); ++cell) {
        area += wildlife.area(cell);
    }
    const double settled = yearly.back();
    CHECK(settled > 2.0 * area);
    CHECK(settled < 10.0 * area);
    CHECK(std::abs(yearly[yearly.size() - 2] - settled) < 0.02 * settled);
    CHECK(wildlife.fields().forage[0] < 100'000.0F);
    CHECK(wildlife.fields().populations[0].fed[0] > 0.5F);
}

TEST_CASE("predators and prey rise and fall over years", "[sim][wildlife]") {
    JobSystem jobs{1};
    Species deer = grazer();
    deer.crowded = 40.0;
    deer.births = 1.2;
    Species wolf{
        .name = "wolf",
        .mass = 40.0,
        .appetite = 2.0,
        .half_fed = 400.0,
        .diet = {Food{.kind = Food::Kind::Prey, .prey = 0}},
        .density = 0.05,
        .crowded = 5.0,
        .births = 1.5,
        .lifespan = 8.0,
        .starving = 0.02,
    };
    wolf.habitat[at(Biome::Grassland)] = 1.0;
    Wildlife wildlife{config({deer, wolf}), grassland(), march_equinox};
    const Steady weather;
    std::vector<double> deer_years;
    std::vector<double> wolf_years;
    for (int year = 1; year <= 40; ++year) {
        wildlife.advance_to(march_equinox + days{365 * year}, weather.conditions(), jobs);
        deer_years.push_back(wildlife.total(0));
        wolf_years.push_back(wildlife.total(1));
    }
    const std::span late{std::span{deer_years}.subspan(15)};
    const auto [low, high] = std::ranges::minmax(late);
    CHECK(high > 1.5 * low);
    int peaks = 0;
    for (std::size_t year = 1; year + 1 < late.size(); ++year) {
        peaks += late[year] > late[year - 1] && late[year] >= late[year + 1] ? 1 : 0;
    }
    CHECK(peaks >= 2);
    CHECK(wolf_years.back() > 0.0);
    CHECK(deer_years.back() > 0.0);
}

TEST_CASE("animals head for better ground and back again", "[sim][wildlife]") {
    JobSystem jobs{1};
    Species deer = grazer();
    // Only movement changes the numbers.
    deer.births = 0.0;
    deer.lifespan = 1e4;
    deer.starving = 0.0;
    deer.exposure = 0.0;
    deer.cold = 0.0;
    deer.speed = 10.0;
    Wildlife wildlife{config({deer}), grassland(), march_equinox};
    const double start = wildlife.total(0);
    Steady weather;
    const auto winter_in = [&](bool north) {
        for (std::uint32_t cell = 0; cell < weather.temperature.size(); ++cell) {
            const bool northern = wildlife.grid().frame(cell).up.z > 0.0;
            weather.temperature[cell] = northern == north ? -30.0F : 15.0F;
        }
    };
    winter_in(true);
    wildlife.advance_to(march_equinox + days{60}, weather.conditions(), jobs);
    CHECK(northern_share(wildlife, 0) < 0.35);
    winter_in(false);
    wildlife.advance_to(march_equinox + days{120}, weather.conditions(), jobs);
    CHECK(northern_share(wildlife, 0) > 0.65);
    CHECK(std::abs(wildlife.total(0) - start) < 1e-3 * start);
}

TEST_CASE("young are born in their season in either hemisphere", "[sim][wildlife]") {
    JobSystem jobs{1};
    Species deer = grazer();
    deer.lifespan = 1e4;
    deer.wander = 0.0;
    deer.speed = 0.0;
    deer.crowded = 1e6;
    deer.breeding = {.start = 30.0, .days = 40.0};
    Wildlife wildlife{config({deer}), grassland(), march_equinox};
    const Steady weather;
    std::uint32_t north = 0;
    std::uint32_t south = 0;
    for (std::uint32_t cell = 0; cell < wildlife.grid().size(); ++cell) {
        (wildlife.grid().frame(cell).up.z > 0.5 ? north : south) = cell;
    }
    REQUIRE(wildlife.grid().frame(south).up.z < -0.5);
    std::set<int> northern_days;
    std::set<int> southern_days;
    for (int day = 1; day <= 365; ++day) {
        const float north_before = wildlife.fields().populations[0].count[north];
        const float south_before = wildlife.fields().populations[0].count[south];
        wildlife.advance_to(march_equinox + days{day}, weather.conditions(), jobs);
        if (wildlife.fields().populations[0].count[north] > north_before) {
            northern_days.insert(day);
        }
        if (wildlife.fields().populations[0].count[south] > south_before) {
            southern_days.insert(day);
        }
    }
    REQUIRE_FALSE(northern_days.empty());
    REQUIRE_FALSE(southern_days.empty());
    CHECK(*northern_days.begin() >= 29);
    CHECK(*northern_days.rbegin() <= 72);
    // Half a year on, give or take the orbit's uneven pace.
    CHECK(*southern_days.begin() >= 29 + 178);
    CHECK(*southern_days.rbegin() <= 72 + 188);
}

TEST_CASE("authored cycles boom in different years in different regions", "[sim][wildlife]") {
    JobSystem jobs{1};
    Species cicada = grazer();
    cicada.name = "cicada";
    cicada.lifespan = 1e4;
    cicada.wander = 0.0;
    cicada.speed = 0.0;
    cicada.crowded = 1e9;
    cicada.births = 0.1;
    cicada.cycle = {.years = 5.0, .boom = 20.0, .width = 1.0, .region_level = 1};
    Wildlife wildlife{config({cicada}), grassland(), march_equinox};
    const Steady weather;
    const auto& count = wildlife.fields().populations[0].count;
    std::set<std::size_t> boom_years;
    for (std::uint32_t cell = 0; cell < count.size(); cell += 16) {
        // Growth over each of five years, for one cell at a time.
        Wildlife one = wildlife;
        std::vector<double> growth;
        for (int year = 1; year <= 5; ++year) {
            const auto before = static_cast<double>(one.fields().populations[0].count[cell]);
            one.advance_to(march_equinox + days{365 * year}, weather.conditions(), jobs);
            growth.push_back(static_cast<double>(one.fields().populations[0].count[cell]) / before);
        }
        const auto [low, high] = std::ranges::minmax_element(growth);
        CHECK(*high - 1.0 > 4.0 * (*low - 1.0));
        boom_years.insert(static_cast<std::size_t>(high - growth.begin()));
        if (boom_years.size() > 1 && cell > 64) {
            break;
        }
    }
    CHECK(boom_years.size() > 1);
}

TEST_CASE("sleeping animals neither eat, breed nor move", "[sim][wildlife]") {
    JobSystem jobs{1};
    Species bear = grazer();
    bear.dormant_below = 0.0;
    bear.speed = 100.0;
    bear.wander = 100.0;
    Wildlife wildlife{config({bear}), grassland(), march_equinox};
    const auto before = wildlife.fields().populations[0].count;
    const Steady cold{-10.0F};
    wildlife.advance_to(march_equinox + days{30}, cold.conditions(), jobs);
    const auto& population = wildlife.fields().populations[0];
    for (std::size_t cell = 0; cell < before.size(); ++cell) {
        CHECK(population.count[cell] < before[cell]);
        CHECK(population.count[cell] > 0.99F * before[cell]);
        CHECK(population.fed[cell] == 1.0F);
    }
}

TEST_CASE("scavengers eat what dies", "[sim][wildlife]") {
    JobSystem jobs{1};
    Species deer = grazer();
    deer.lifespan = 1.0;
    Species crow{
        .name = "crow",
        .moves = hog::sim::Moves::Air,
        .mass = 0.5,
        .appetite = 0.1,
        .half_fed = 5.0,
        .diet = {Food{.kind = Food::Kind::Carrion}},
        .density = 1.0,
    };
    crow.habitat[at(Biome::Grassland)] = 1.0;
    Wildlife wildlife{config({deer, crow}), grassland(), march_equinox};
    CHECK(wildlife.fields().carrion[0] == 0.0F);
    const Steady weather;
    wildlife.advance_to(march_equinox + days{20}, weather.conditions(), jobs);
    CHECK(wildlife.fields().carrion[0] > 0.0F);
    CHECK(wildlife.fields().populations[1].fed[0] > 0.3F);
}

TEST_CASE("animals can be taken from the population and put back", "[sim][wildlife]") {
    Wildlife wildlife{config({grazer()}), grassland(), march_equinox};
    const float there = wildlife.fields().populations[0].count[5];
    CHECK(wildlife.take(0, 5, 2.0F) == 2.0F);
    CHECK(wildlife.fields().populations[0].count[5] == there - 2.0F);
    CHECK(wildlife.take(0, 5, 1e9F) == there - 2.0F);
    CHECK(wildlife.fields().populations[0].count[5] == 0.0F);
    wildlife.add(0, 5, 3.0F);
    CHECK(wildlife.fields().populations[0].count[5] == 3.0F);
}

TEST_CASE("wildlife is the same on any number of threads", "[sim][wildlife]") {
    JobSystem one{1};
    JobSystem many{4};
    Species wolf = grazer();
    wolf.name = "wolf";
    wolf.diet = {Food{.kind = Food::Kind::Prey, .prey = 0}, Food{.kind = Food::Kind::Carrion}};
    wolf.density = 0.1;
    Wildlife a{config({grazer(), wolf}), grassland(), march_equinox};
    Wildlife b = a;
    Steady weather;
    for (std::size_t cell = 0; cell < weather.temperature.size(); ++cell) {
        weather.temperature[cell] = static_cast<float>(cell % 50) - 20.0F;
    }
    a.advance_to(march_equinox + days{100}, weather.conditions(), one);
    b.advance_to(march_equinox + days{100}, weather.conditions(), many);
    CHECK(a.fields() == b.fields());
}

TEST_CASE("wildlife lives on the planet under its weather", "[sim][wildlife]") {
    const hog::planet::World world{hog::planet::PlanetConfig{.seed = 1, .radius = 115'000.0}};
    const hog::sim::CellGrid grid{level};
    const auto habitat = hog::sim::habitat_for(grid, world);
    REQUIRE(habitat.size() == grid.size());
    const auto oceans = std::ranges::count_if(habitat, &Habitat::ocean);
    CHECK(oceans > 0);
    CHECK(std::cmp_less(oceans, habitat.size()));
    for (const Habitat& one : habitat) {
        CHECK(one.ocean == (one.biome == Biome::Ocean));
    }

    const hog::sim::Weather weather{hog::sim::WeatherConfig{.level = level},
                                    hog::sim::ground_for(grid, world), march_equinox};
    const Conditions conditions = hog::sim::conditions_of(weather);
    CHECK(conditions.temperature.size() == grid.size());
    CHECK(conditions.precipitation.data() == weather.fields().recent_precipitation.data());

    // Land animals stay off the ocean.
    JobSystem jobs{1};
    Wildlife wildlife{config({grazer()}), habitat, march_equinox};
    wildlife.advance_to(march_equinox + days{30}, conditions, jobs);
    for (std::uint32_t cell = 0; cell < grid.size(); ++cell) {
        if (habitat[cell].ocean) {
            CHECK(wildlife.fields().populations[0].count[cell] == 0.0F);
        }
    }
}
