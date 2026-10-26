#include <hog/core/bytes.hpp>
#include <hog/jobs/job_system.hpp>
#include <hog/planet/world.hpp>
#include <hog/sim/weather.hpp>
#include <hog/sim/wildlife.hpp>
#include <hog/testing/quick.hpp>
#include <hog/testing/worlds.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <set>
#include <span>
#include <string>
#include <tuple>
#include <vector>

using namespace std::chrono;
using Catch::Matchers::WithinRel;
using hog::GameTime;
using hog::jobs::JobSystem;
using hog::planet::Biome;
using hog::sim::Conditions;
using hog::sim::EcologyEvent;
using hog::sim::Food;
using hog::sim::Habitat;
using hog::sim::Recovery;
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

WildlifeConfig config(std::vector<Species> species, int grid_level = level) {
    WildlifeConfig config{.level = grid_level};
    config.ecology.forage.capacity[at(Biome::Grassland)] = 100'000.0;
    config.ecology.species = std::move(species);
    return config;
}

std::vector<Habitat> grassland(int grid_level = level) {
    return std::vector<Habitat>(hog::sim::CellGrid{grid_level}.size(), Habitat{});
}

// Weather that stays the same everywhere.
struct Steady {
    std::vector<float> temperature;
    std::vector<float> precipitation;

    explicit Steady(float warmth = 15.0F, float wet = 3.0F, int grid_level = level)
        : temperature(hog::sim::CellGrid{grid_level}.size(), warmth),
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

constexpr std::uint32_t island_a = 0;
constexpr std::uint32_t island_b = 10;

// Ocean everywhere but two regions of grassland, too far apart for land animals to cross.
std::vector<Habitat> islands() {
    const Wildlife probe{config({}), grassland(), march_equinox};
    std::vector<Habitat> habitat = grassland();
    for (std::uint32_t cell = 0; cell < habitat.size(); ++cell) {
        const std::uint32_t region = probe.region(cell);
        if (region != island_a && region != island_b) {
            habitat[cell] = Habitat{.biome = Biome::Ocean, .ocean = true};
        }
    }
    return habitat;
}

double in_region(const Wildlife& wildlife, std::uint32_t species, std::uint32_t region) {
    double sum = 0.0;
    for (std::uint32_t cell = 0; cell < wildlife.grid().size(); ++cell) {
        if (wildlife.region(cell) == region) {
            sum += static_cast<double>(wildlife.fields().populations[species].count[cell]);
        }
    }
    return sum;
}

void clear(Wildlife& wildlife, std::uint32_t species, std::uint32_t region) {
    for (std::uint32_t cell = 0; cell < wildlife.grid().size(); ++cell) {
        if (wildlife.region(cell) == region) {
            wildlife.take(species, cell, 1e9F);
        }
    }
}

// Kills `share` of the species in each of the region's cells.
void hunt(Wildlife& wildlife, std::uint32_t region, hog::sim::Hunter hunter, float share) {
    for (std::uint32_t cell = 0; cell < wildlife.grid().size(); ++cell) {
        if (wildlife.region(cell) == region) {
            wildlife.kill(0, cell, share * wildlife.fields().populations[0].count[cell], hunter);
        }
    }
}

std::vector<EcologyEvent> of_kind(const std::vector<EcologyEvent>& events,
                                  EcologyEvent::Kind kind) {
    std::vector<EcologyEvent> found;
    std::ranges::copy_if(events, std::back_inserter(found),
                         [&](const EcologyEvent& event) { return event.kind == kind; });
    return found;
}

std::vector<hog::sim::Hunter> hunters(const EcologyEvent& event) {
    std::vector<hog::sim::Hunter> named;
    std::ranges::transform(event.hunters, std::back_inserter(named),
                           &hog::sim::Contributor::hunter);
    return named;
}

double kills_by(const Wildlife& wildlife, std::uint32_t region, hog::sim::Hunter hunter) {
    double kills = 0.0;
    for (const auto& one : wildlife.kills(0, region)) {
        kills += one.hunter == hunter ? one.kills : 0.0;
    }
    return kills;
}

Species returning(double earliest, double latest) {
    Species deer = grazer();
    deer.recovery.earliest = earliest;
    deer.recovery.latest = latest;
    return deer;
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
    const int coarse = hog::testing::full_or(level, level - 1);
    Wildlife wildlife{config({deer, wolf}, coarse), grassland(coarse), march_equinox};
    const Steady weather{15.0F, 3.0F, coarse};
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

TEST_CASE("fast animals settle onto better ground without overshooting", "[sim][wildlife]") {
    JobSystem jobs{1};
    Species raven = grazer();
    raven.births = 0.0;
    raven.lifespan = 1e4;
    raven.starving = 0.0;
    raven.speed = 40.0;
    Wildlife wildlife{config({raven}), grassland(), march_equinox};
    constexpr std::uint32_t crowded = 5;
    wildlife.add(0, crowded, 10'000.0F);
    const Steady weather;
    // Ground overshot one step is better left the next, so the count would swing up and down for
    // good; settling, it only does at first.
    wildlife.advance_to(march_equinox + days{14}, weather.conditions(), jobs);
    int swings = 0;
    float before = wildlife.fields().populations[0].count[crowded];
    float change = 0.0F;
    for (int day = 15; day <= 30; ++day) {
        wildlife.advance_to(march_equinox + days{day}, weather.conditions(), jobs);
        const float now = wildlife.fields().populations[0].count[crowded];
        swings += (now - before) * change < 0.0F ? 1 : 0;
        change = now - before;
        before = now;
    }
    CHECK(swings == 0);
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

TEST_CASE("only real hunger kills", "[sim][wildlife]") {
    JobSystem jobs{1};
    Species deer = grazer();
    deer.births = 0.0;
    deer.lifespan = 1e4;
    deer.wander = 0.0;
    deer.speed = 0.0;
    deer.starving = 0.05;
    deer.hungry_below = 0.5;
    // Forage that fills them about two thirds of the way, then about a quarter.
    deer.half_fed = 50'000.0;
    Wildlife fed{config({deer}), grassland(), march_equinox};
    deer.half_fed = 300'000.0;
    Wildlife hungry{config({deer}), grassland(), march_equinox};
    const double start = fed.total(0);
    const Steady weather;
    fed.advance_to(march_equinox + days{30}, weather.conditions(), jobs);
    hungry.advance_to(march_equinox + days{30}, weather.conditions(), jobs);
    CHECK(fed.fields().populations[0].fed[0] > 0.5F);
    CHECK(fed.total(0) > 0.999 * start);
    CHECK(hungry.fields().populations[0].fed[0] < 0.5F);
    CHECK(hungry.total(0) < 0.98 * start);
}

TEST_CASE("animals stray only onto ground they can live on", "[sim][wildlife]") {
    JobSystem jobs{1};
    Species crow = grazer();
    crow.name = "crow";
    crow.moves = hog::sim::Moves::Air;
    crow.wander = 50.0;
    crow.births = 0.0;
    auto habitat = grassland();
    const hog::sim::CellGrid grid{level};
    for (std::uint32_t cell = 0; cell < grid.size(); ++cell) {
        if (grid.frame(cell).up.z > 0.0) {
            habitat[cell] = Habitat{.biome = Biome::Ocean, .ocean = true};
        }
    }
    Wildlife wildlife{config({crow}), habitat, march_equinox};
    const Steady weather;
    wildlife.advance_to(march_equinox + days{30}, weather.conditions(), jobs);
    CHECK(northern_share(wildlife, 0) < 1e-6);
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

TEST_CASE("emptied ground fills back from its neighbours", "[sim][wildlife][recovery]") {
    JobSystem jobs{1};
    Species deer = grazer();
    deer.recovery.founders = 0.0;
    Wildlife wildlife{config({deer}), grassland(), march_equinox};
    const Steady weather;
    wildlife.advance_to(march_equinox + days{365}, weather.conditions(), jobs);
    const double settled = in_region(wildlife, 0, 20);
    clear(wildlife, 0, 20);
    wildlife.advance_to(march_equinox + days{2 * 365}, weather.conditions(), jobs);
    CHECK(in_region(wildlife, 0, 20) > 0.5 * settled);
}

TEST_CASE("a species wiped from a region returns as founders", "[sim][wildlife][recovery]") {
    JobSystem jobs{1};
    Wildlife wildlife{config({returning(1.0, 2.0)}), islands(), march_equinox};
    const Steady weather;
    wildlife.advance_to(march_equinox + days{30}, weather.conditions(), jobs);
    const double other_island = in_region(wildlife, 0, island_b);
    clear(wildlife, 0, island_a);
    wildlife.advance_to(march_equinox + days{31}, weather.conditions(), jobs);
    const auto& presence = wildlife.presence(0);
    CHECK(presence.alive);
    CHECK(presence.emptied[island_a] == wildlife.time());
    CHECK_FALSE(presence.emptied[island_b]);
    CHECK(in_region(wildlife, 0, island_b) > 0.5 * other_island);

    wildlife.advance_to(march_equinox + days{31 + 360}, weather.conditions(), jobs);
    CHECK(in_region(wildlife, 0, island_a) == 0.0);
    wildlife.advance_to(march_equinox + days{31 + (2 * 366)}, weather.conditions(), jobs);
    CHECK(in_region(wildlife, 0, island_a) > 0.0);
    CHECK_FALSE(wildlife.presence(0).emptied[island_a]);
}

TEST_CASE("rivals keep an emptied niche from founders", "[sim][wildlife][recovery]") {
    JobSystem jobs{1};
    Species elk = returning(1.0, 2.0);
    elk.name = "elk";
    Wildlife wildlife{config({returning(1.0, 2.0), elk}), islands(), march_equinox};
    const Steady weather;
    clear(wildlife, 0, island_a);
    wildlife.advance_to(march_equinox + days{3 * 365}, weather.conditions(), jobs);
    CHECK(in_region(wildlife, 0, island_a) == 0.0);
    CHECK(wildlife.presence(0).emptied[island_a]);
    CHECK(in_region(wildlife, 1, island_a) > 0.0);
    CHECK(in_region(wildlife, 0, island_b) > 0.0);

    // Once the rivals are gone too, both are free to come back.
    clear(wildlife, 1, island_a);
    wildlife.advance_to(march_equinox + days{6 * 365}, weather.conditions(), jobs);
    CHECK(in_region(wildlife, 0, island_a) + in_region(wildlife, 1, island_a) > 0.0);
}

TEST_CASE("a species gone from everywhere waits to be reintroduced", "[sim][wildlife][recovery]") {
    JobSystem jobs{1};
    Wildlife wildlife{config({returning(1.0, 2.0)}), islands(), march_equinox};
    const Steady weather;
    clear(wildlife, 0, island_a);
    clear(wildlife, 0, island_b);
    wildlife.advance_to(march_equinox + days{1}, weather.conditions(), jobs);
    const GameTime gone = wildlife.time();
    CHECK_FALSE(wildlife.presence(0).alive);
    CHECK(wildlife.presence(0).extinct == gone);

    wildlife.advance_to(march_equinox + days{15 * 365}, weather.conditions(), jobs);
    CHECK(wildlife.total(0) == 0.0);
    CHECK(wildlife.presence(0).extinct == gone);

    // Brought back to one island, it's alive again, and the other's wait starts over.
    const GameTime back = march_equinox + days{(15 * 365) + 1};
    for (std::uint32_t cell = 0; cell < wildlife.grid().size(); ++cell) {
        if (wildlife.region(cell) == island_a) {
            wildlife.add(0, cell, 20.0F);
        }
    }
    wildlife.advance_to(back, weather.conditions(), jobs);
    CHECK(wildlife.presence(0).alive);
    CHECK_FALSE(wildlife.presence(0).extinct);
    CHECK(wildlife.presence(0).emptied[island_b] == wildlife.time());
    wildlife.advance_to(back + days{300}, weather.conditions(), jobs);
    CHECK(in_region(wildlife, 0, island_b) == 0.0);
    wildlife.advance_to(back + days{2 * 366}, weather.conditions(), jobs);
    CHECK(in_region(wildlife, 0, island_b) > 0.0);
}

TEST_CASE("relicts of an extinct species turn up long after", "[sim][wildlife][recovery]") {
    JobSystem jobs{1};
    Species deer = grazer();
    deer.recovery.after_extinction = Recovery::After::Relict;
    deer.recovery.extinct_earliest = 1.0;
    deer.recovery.extinct_latest = 2.0;
    Wildlife wildlife{config({deer}), grassland(), march_equinox};
    const Steady weather;
    for (std::uint32_t cell = 0; cell < wildlife.grid().size(); ++cell) {
        wildlife.take(0, cell, 1e9F);
    }
    wildlife.advance_to(march_equinox + days{360}, weather.conditions(), jobs);
    CHECK(wildlife.total(0) == 0.0);
    CHECK(wildlife.presence(0).extinct);
    wildlife.advance_to(march_equinox + days{2 * 366}, weather.conditions(), jobs);
    CHECK(wildlife.total(0) > 0.0);
    CHECK_FALSE(wildlife.presence(0).extinct);
}

TEST_CASE("a successor takes the place of an extinct species", "[sim][wildlife][recovery]") {
    JobSystem jobs{1};
    Species deer = grazer();
    deer.recovery.after_extinction = Recovery::After::Successor;
    deer.recovery.successor = 1;
    deer.recovery.extinct_earliest = 1.0;
    deer.recovery.extinct_latest = 2.0;
    Species elk = grazer();
    elk.name = "elk";
    elk.density = 0.0;
    Wildlife wildlife{config({deer, elk}), grassland(), march_equinox};
    // Never having lived isn't dying out.
    CHECK_FALSE(wildlife.presence(1).alive);
    CHECK_FALSE(wildlife.presence(1).extinct);

    const Steady weather;
    for (std::uint32_t cell = 0; cell < wildlife.grid().size(); ++cell) {
        wildlife.take(0, cell, 1e9F);
    }
    wildlife.advance_to(march_equinox + days{360}, weather.conditions(), jobs);
    CHECK(wildlife.total(1) == 0.0);
    CHECK_FALSE(wildlife.presence(1).extinct);
    wildlife.advance_to(march_equinox + days{2 * 366}, weather.conditions(), jobs);
    CHECK(wildlife.total(0) == 0.0);
    CHECK(wildlife.presence(0).extinct);
    CHECK(wildlife.presence(0).succeeded);
    CHECK(wildlife.total(1) > 0.0);
    CHECK(wildlife.presence(1).alive);
}

TEST_CASE("kills are remembered against their hunters, fading", "[sim][wildlife][events]") {
    JobSystem jobs{1};
    Wildlife wildlife{config({grazer()}), grassland(), march_equinox};
    const std::uint32_t region = wildlife.region(5);
    CHECK(wildlife.kill(0, 5, 3.0F, 7) == 3.0F);
    wildlife.kill(0, 5, 1.0F, 8);
    wildlife.kill(0, 5, 2.0F, 7);
    wildlife.kill(0, 5, 0.015F, 9);
    // Animals taken some other way are no one's kills.
    wildlife.take(0, 5, 4.0F);
    CHECK(wildlife.kills(0, region).size() == 3);
    CHECK(kills_by(wildlife, region, 7) == 5.0);
    CHECK(kills_by(wildlife, region, 8) == 1.0);
    CHECK(wildlife.kill(0, 5, 1e9F, 8) < 1e9F);

    const auto steps = wildlife.advance_to(march_equinox + days{730}, Steady{}.conditions(), jobs);
    const double faded = std::exp2(-static_cast<double>(steps) / hog::sim::days_per_year / 2.0);
    CHECK_THAT(kills_by(wildlife, region, 7), WithinRel(5.0 * faded, 1e-9));
    // The fewest are forgotten.
    CHECK(wildlife.kills(0, region).size() == 2);
}

TEST_CASE("a region hunted out reports its collapse, its loss and its return",
          "[sim][wildlife][events]") {
    JobSystem jobs{1};
    Species deer = returning(1.0, 2.0);
    deer.events.baseline_years = 0.5;
    Wildlife wildlife{config({deer}), islands(), march_equinox};
    const Steady weather;
    wildlife.advance_to(march_equinox + days{200}, weather.conditions(), jobs);
    std::ignore = wildlife.drain_events();

    hunt(wildlife, island_a, 7, 0.6F);
    hunt(wildlife, island_a, 8, 0.75F);
    // Too few kills to be named.
    hunt(wildlife, island_a, 9, 0.5F);
    wildlife.advance_to(march_equinox + days{201}, weather.conditions(), jobs);
    auto events = wildlife.drain_events();
    REQUIRE(events.size() == 1);
    CHECK(events[0].kind == EcologyEvent::Kind::Collapse);
    CHECK(events[0].region == island_a);
    CHECK(events[0].at == wildlife.time());
    CHECK(events[0].count < 0.2 * events[0].baseline);
    CHECK(hunters(events[0]) == std::vector<hog::sim::Hunter>{7, 8});

    hunt(wildlife, island_a, 8, 1.0F);
    wildlife.advance_to(march_equinox + days{202}, weather.conditions(), jobs);
    events = wildlife.drain_events();
    REQUIRE(events.size() == 1);
    CHECK(events[0].kind == EcologyEvent::Kind::LocalExtinction);
    CHECK(events[0].count == 0.0);
    CHECK(hunters(events[0]) == std::vector<hog::sim::Hunter>{7, 8});

    // Founders come back to the island, and that's news of its own.
    wildlife.advance_to(march_equinox + days{202 + (3 * 365)}, weather.conditions(), jobs);
    events = of_kind(wildlife.drain_events(), EcologyEvent::Kind::Return);
    REQUIRE(events.size() == 1);
    CHECK(events[0].region == island_a);
    CHECK(events[0].hunters.empty());
}

TEST_CASE("a species hunted out everywhere names its hunters", "[sim][wildlife][events]") {
    JobSystem jobs{1};
    Wildlife wildlife{config({returning(1.0, 2.0)}), islands(), march_equinox};
    const Steady weather;
    hunt(wildlife, island_a, 7, 1.0F);
    hunt(wildlife, island_b, 8, 1.0F);
    wildlife.advance_to(march_equinox + days{1}, weather.conditions(), jobs);
    const auto events = wildlife.drain_events();
    // Losses are news from the start; there's no baseline to settle for them.
    REQUIRE(events.size() == 3);
    CHECK(events[0].kind == EcologyEvent::Kind::LocalExtinction);
    CHECK(events[1].kind == EcologyEvent::Kind::LocalExtinction);
    CHECK(hunters(events[0]) == std::vector<hog::sim::Hunter>{7});
    const EcologyEvent& gone = events[2];
    CHECK(gone.kind == EcologyEvent::Kind::Extinction);
    CHECK(gone.region == EcologyEvent::everywhere);
    CHECK(gone.count == 0.0);
    CHECK(gone.baseline > 1'000.0);
    CHECK(gone.hunters.size() == 2);

    wildlife.add(0, 0, 50.0F);
    wildlife.advance_to(march_equinox + days{2}, weather.conditions(), jobs);
    const auto back = of_kind(wildlife.drain_events(), EcologyEvent::Kind::Return);
    REQUIRE(back.size() == 2);
    CHECK(back[1].region == EcologyEvent::everywhere);
}

TEST_CASE("overpopulation is reported once the baseline has settled", "[sim][wildlife][events]") {
    JobSystem jobs{1};
    Species deer = grazer();
    deer.events.baseline_years = 1.0;
    Wildlife wildlife{config({deer}), islands(), march_equinox};
    const Steady weather;
    const auto flood = [&] {
        for (std::uint32_t cell = 0; cell < wildlife.grid().size(); ++cell) {
            if (wildlife.region(cell) == island_a) {
                wildlife.add(0, cell, 4.0F * wildlife.fields().populations[0].count[cell]);
            }
        }
    };
    hunt(wildlife, island_a, 5, 0.1F);
    flood();
    wildlife.advance_to(march_equinox + days{1}, weather.conditions(), jobs);
    CHECK(of_kind(wildlife.drain_events(), EcologyEvent::Kind::Overpopulation).empty());

    wildlife.advance_to(march_equinox + days{2 * 365}, weather.conditions(), jobs);
    std::ignore = wildlife.drain_events();
    flood();
    wildlife.advance_to(march_equinox + days{(2 * 365) + 1}, weather.conditions(), jobs);
    const auto events = wildlife.drain_events();
    REQUIRE(events.size() == 1);
    CHECK(events[0].kind == EcologyEvent::Kind::Overpopulation);
    CHECK(events[0].region == island_a);
    CHECK(hunters(events[0]) == std::vector<hog::sim::Hunter>{5});
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

TEST_CASE("wildlife saves and loads", "[sim][wildlife]") {
    JobSystem jobs{1};
    Species wolf = grazer();
    wolf.name = "wolf";
    wolf.diet = {Food{.kind = Food::Kind::Prey, .prey = 0}};
    wolf.density = 0.1;
    Wildlife saved{config({grazer(), wolf}), grassland(), march_equinox};
    const Steady weather;
    saved.advance_to(march_equinox + days{40}, weather.conditions(), jobs);
    saved.kill(0, 5, 2.0F, 7);
    const auto bytes = saved.save();

    Wildlife loaded{config({grazer(), wolf}), grassland(), march_equinox + days{400}};
    REQUIRE(loaded.load(Wildlife::save_version, bytes));
    CHECK(loaded.time() == saved.time());
    CHECK(loaded.fields() == saved.fields());
    CHECK(loaded.presence(0) == saved.presence(0));
    CHECK(loaded.presence(1) == saved.presence(1));
    for (std::uint32_t region = 0; region < saved.region_count(); ++region) {
        CHECK(loaded.baseline(0, region) == saved.baseline(0, region));
        CHECK(std::ranges::equal(loaded.kills(0, region), saved.kills(0, region)));
    }
    CHECK(kills_by(loaded, saved.region(5), 7) == 2.0);
    saved.advance_to(march_equinox + days{50}, weather.conditions(), jobs);
    loaded.advance_to(march_equinox + days{50}, weather.conditions(), jobs);
    CHECK(loaded.fields() == saved.fields());

    // Species are found by name: the wolf moved, the deer left, and the crow is new.
    Species crow = grazer();
    crow.name = "crow";
    Wildlife changed{config({wolf, crow}), grassland(), march_equinox};
    const auto crows = changed.fields().populations[1];
    REQUIRE(changed.load(Wildlife::save_version, bytes));
    Wildlife reloaded{config({grazer(), wolf}), grassland(), march_equinox};
    REQUIRE(reloaded.load(Wildlife::save_version, bytes));
    CHECK(changed.fields().populations[0] == reloaded.fields().populations[1]);
    CHECK(changed.fields().populations[1] == crows);

    Wildlife finer{WildlifeConfig{.level = level + 1}, std::vector<Habitat>(6U << 8U),
                   march_equinox};
    const auto other_level = finer.load(Wildlife::save_version, bytes);
    REQUIRE_FALSE(other_level);
    CHECK(other_level.error().code == hog::ErrorCode::InvalidArgument);
    const auto newer = reloaded.load(Wildlife::save_version + 1, bytes);
    REQUIRE_FALSE(newer);
    CHECK(newer.error().code == hog::ErrorCode::Unsupported);

    const auto before = reloaded.fields();
    const std::span<const std::byte> all{bytes};
    CHECK_FALSE(reloaded.load(Wildlife::save_version, all.first(all.size() - 1)));
    auto longer = bytes;
    longer.push_back(std::byte{0});
    CHECK_FALSE(reloaded.load(Wildlife::save_version, longer));
    CHECK(reloaded.fields() == before);
}

TEST_CASE("wildlife loads saves from before it kept presence", "[sim][wildlife][recovery]") {
    Wildlife wildlife{config({grazer()}), grassland(), march_equinox};
    const std::uint32_t cells = wildlife.grid().size();
    const GameTime saved = march_equinox + days{10};
    hog::ByteWriter out;
    out.write(static_cast<std::uint8_t>(level));
    out.write(static_cast<std::uint64_t>(saved.time_since_epoch().count()));
    const auto floats = [&](float value) {
        for (std::uint32_t cell = 0; cell < cells; ++cell) {
            out.write(std::bit_cast<std::uint32_t>(value));
        }
    };
    floats(1'000.0F);
    floats(0.0F);
    out.write(std::uint32_t{1});
    const std::string name = "deer";
    out.write(static_cast<std::uint16_t>(name.size()));
    out.write_bytes(std::as_bytes(std::span{name}));
    for (std::uint32_t cell = 0; cell < cells; ++cell) {
        out.write(std::bit_cast<std::uint32_t>(wildlife.region(cell) == 3 ? 0.0F : 5.0F));
    }
    floats(1.0F);

    const auto bytes = std::move(out).take();
    REQUIRE(wildlife.load(1, bytes));
    CHECK(wildlife.presence(0).alive);
    CHECK(wildlife.presence(0).emptied[3] == saved);
    CHECK_FALSE(wildlife.presence(0).emptied[4]);
    CHECK(wildlife.baseline(0, 3) == 0.0);
    CHECK(wildlife.baseline(0, 4) > 0.0);
    CHECK(wildlife.kills(0, 4).empty());
}

TEST_CASE("wildlife lives on the planet under its weather", "[sim][wildlife]") {
    const hog::planet::World world{hog::planet::PlanetConfig{
        .seed = 1, .radius = 115'000.0, .erosion_cell = hog::testing::coarse_erosion_cell}};
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
