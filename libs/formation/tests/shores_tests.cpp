#include <hog/formation/shores.hpp>
#include <hog/formation/world.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <vector>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using hog::DVec3;
using hog::formation::Bedrock;
using hog::formation::Body;
using hog::formation::Climate;
using hog::formation::Ice;
using hog::formation::Outcrop;
using hog::formation::Rivers;
using hog::formation::Rock;
using hog::formation::Sediment;
using hog::formation::Shores;
using hog::formation::shores_on;
using hog::formation::Sphere;
using hog::formation::wave_height;
using hog::formation::wave_period;
using hog::formation::World;

namespace {

constexpr double earth_radius = 6'371'000.0;

double angle(const DVec3& a, const DVec3& b) {
    return std::acos(std::clamp(glm::dot(a, b), -1.0, 1.0));
}

// A planet of granite land standing 50 m over a shallow sea where `sea` says, a mild climate and
// an east wind, nothing worn and nothing brought down to the coast.
struct Coast {
    Sphere sphere{6, 3};
    Body body;
    Climate climate;
    Rivers rivers;
    Bedrock bedrock;
    Outcrop outcrop;
    Ice ice;
    Sediment sediment;

    explicit Coast(const std::function<bool(const DVec3&)>& sea) {
        const std::uint32_t size = sphere.size();
        climate.temperature.assign(size, 15.0);
        climate.wind.assign(size, DVec3{0.0});
        rivers.height.assign(size, 50.0);
        rivers.receiver.resize(size);
        rivers.wear.assign(size, 0.0);
        bedrock.layering.assign(size, 0.5);
        bedrock.soluble.assign(size, 0.0);
        outcrop.rock.assign(size, Rock::Basement);
        ice.sea_ice.assign(size, 0.0);
        ice.thickness.assign(size, 0.0);
        ice.till.assign(size, 0.0);
        sediment.settling.assign(size, 0.0);
        for (std::uint32_t point = 0; point < size; ++point) {
            const DVec3 here = sphere.point(point);
            const DVec3 east = glm::cross(DVec3{0.0, 0.0, 1.0}, here);
            climate.wind[point] = glm::length(east) > 1e-6 ? glm::normalize(east) : DVec3{0.0};
            if (sea(here)) {
                rivers.height[point] = -100.0;
                rivers.receiver[point] = point;
                continue;
            }
            // Draining towards the south, where the seas all are.
            std::uint32_t lowest = point;
            for (const std::uint32_t near : sphere.neighbors(point)) {
                if (sphere.point(near).z < sphere.point(lowest).z) {
                    lowest = near;
                }
            }
            rivers.receiver[point] = lowest;
        }
    }

    [[nodiscard]] Shores shores() const {
        return shores_on(sphere, climate, rivers, bedrock, outcrop, ice, sediment, body);
    }

    [[nodiscard]] bool in_sea(std::uint32_t point) const { return rivers.receiver[point] == point; }

    // Sea points with land beside them.
    [[nodiscard]] bool on_shore(std::uint32_t point) const {
        return in_sea(point) &&
               std::ranges::any_of(sphere.neighbors(point),
                                   [&](std::uint32_t near) { return !in_sea(near); });
    }

    [[nodiscard]] double length() const { return sphere.spacing() * earth_radius; }
};

// The mean of `field` over shore points `where` says.
double mean_on_shore(const Coast& coast, const std::vector<double>& field,
                     const std::function<bool(const DVec3&)>& where) {
    double sum = 0.0;
    int count = 0;
    for (std::uint32_t point = 0; point < coast.sphere.size(); ++point) {
        if (coast.on_shore(point) && where(coast.sphere.point(point))) {
            sum += field[point];
            ++count;
        }
    }
    REQUIRE(count > 0);
    return sum / count;
}

// Where the sound lies: due east, a little north.
constexpr DVec3 sound_at{0.894427190999916, 0.0, 0.447213595499958};
constexpr double sound_radius = 0.03;

bool south_or_sound(const DVec3& at) {
    return at.z < 0.0 || angle(at, sound_at) < sound_radius;
}

bool in_sound(const DVec3& at) {
    return angle(at, sound_at) < 2.0 * sound_radius;
}

bool on_open_coast(const DVec3& at) {
    return std::abs(at.z) < 0.1;
}

} // namespace

TEST_CASE("waves grow with the open water the wind blows over, until the sea is fully grown",
          "[formation][shores]") {
    const Body body;
    CHECK(wave_height(0.0, body) == 0.0);
    CHECK(wave_height(10'000.0, body) < wave_height(100'000.0, body));
    // A 20 m/s storm's sea is fully grown, near 10 m, over about a thousand kilometres.
    CHECK_THAT(wave_height(5.0e6, body), WithinAbs(9.9, 0.1));
    CHECK(wave_height(5.0e6, body) == wave_height(2.0e6, body));
}

TEST_CASE("a fully grown sea's waves come about fifteen seconds apart, smaller ones sooner",
          "[formation][shores]") {
    const Body body;
    // A Pierson-Moskowitz sea peaks at 0.877 g / U radians a second.
    const double grown = wave_height(5.0e6, body);
    CHECK_THAT(wave_period(grown, body),
               WithinRel(2.0 * 3.141592653589793 * body.storm_wind / (0.877 * body.gravity), 1e-6));
    CHECK(wave_period(1.0, body) < wave_period(grown, body));
    CHECK(wave_period(0.0, body) == 0.0);
}

TEST_CASE("an ordinary day's waves are smaller than storms', wherever any come in",
          "[formation][shores]") {
    const Coast coast{south_or_sound};
    const Shores shores = coast.shores();
    int coasts = 0;
    for (std::uint32_t point = 0; point < coast.sphere.size(); ++point) {
        if (coast.on_shore(point)) {
            ++coasts;
            CHECK(shores.usual[point] > 0.0);
            CHECK(shores.usual[point] < 0.5 * shores.waves[point]);
        }
    }
    CHECK(coasts > 0);
}

TEST_CASE("a sound's coasts get smaller waves than the open ocean's", "[formation][shores]") {
    const Coast coast{south_or_sound};
    const Shores shores = coast.shores();
    const double sound = mean_on_shore(coast, shores.waves, in_sound);
    const double open = mean_on_shore(coast, shores.waves, on_open_coast);
    CHECK(open > 4.0);
    CHECK(sound < 0.75 * open);
}

TEST_CASE("sea ice keeps the waves off, the land's ice keeps them from the rock",
          "[formation][shores]") {
    Coast coast{[](const DVec3& at) { return at.x < 0.0; }};
    for (std::uint32_t point = 0; point < coast.sphere.size(); ++point) {
        const double z = coast.sphere.point(point).z;
        coast.ice.sea_ice[point] = coast.in_sea(point) && z > 0.5 ? 1.0 : 0.0;
        coast.ice.thickness[point] = !coast.in_sea(point) && z < -0.5 ? 100.0 : 0.0;
    }
    const Shores shores = coast.shores();
    for (std::uint32_t point = 0; point < coast.sphere.size(); ++point) {
        const double z = coast.sphere.point(point).z;
        if (coast.on_shore(point) && z > 0.5) {
            CHECK(shores.waves[point] == 0.0);
        }
        if (!coast.in_sea(point) && z < -0.5) {
            CHECK(shores.retreat[point] == 0.0);
        }
    }
    CHECK(mean_on_shore(coast, shores.waves, [](const DVec3& at) { return std::abs(at.z) < 0.3; }) >
          4.0);
}

TEST_CASE("a lava coast's beach is dark, a granite coast's pale", "[formation][shores]") {
    Coast coast{[](const DVec3& at) { return at.z < 0.0; }};
    coast.body.shell_sand = 0.0;
    for (std::uint32_t point = 0; point < coast.sphere.size(); ++point) {
        if (coast.sphere.point(point).x > 0.0) {
            coast.outcrop.rock[point] = Rock::Lava;
        }
    }
    const Shores shores = coast.shores();
    const auto lava = [](const DVec3& at) { return on_open_coast(at) && at.x > 0.3; };
    const auto granite = [](const DVec3& at) { return on_open_coast(at) && at.x < -0.3; };
    CHECK(mean_on_shore(coast, shores.volcanic, lava) > 0.3);
    CHECK(mean_on_shore(coast, shores.quartz, lava) < 0.01);
    CHECK(mean_on_shore(coast, shores.quartz, granite) > 0.3);
    CHECK(mean_on_shore(coast, shores.volcanic, granite) < 0.01);
    // Hard rock breaks into stones the surf rounds; soft rock wears faster, into finer stuff.
    CHECK(mean_on_shore(coast, shores.shingle, granite) > 0.1);
}

TEST_CASE("lava's dark sand breaks down along the shore, staying by the lava it came from",
          "[formation][shores]") {
    // A short stretch of lava coast on a granite one.
    // The drift carrying sand far, so it's breaking down that keeps the lava's to it.
    Coast coast{[](const DVec3& at) { return at.z < 0.0; }};
    coast.body.shell_sand = 0.0;
    coast.body.drift_reach = 1.0e6;
    const DVec3 lava_at{1.0, 0.0, 0.0};
    for (std::uint32_t point = 0; point < coast.sphere.size(); ++point) {
        if (angle(coast.sphere.point(point), lava_at) < 0.04) {
            coast.outcrop.rock[point] = Rock::Lava;
        }
    }
    const auto by_lava = [&](const DVec3& at) { return angle(at, lava_at) < 0.04; };
    // Several of the shore's points along, out of reach of the lava's own cliffs.
    const auto along = [&](const DVec3& at) {
        return on_open_coast(at) && angle(at, lava_at) > 0.1 && angle(at, lava_at) < 0.2;
    };
    const Shores shores = coast.shores();
    coast.body.volcanic_reach = 0.0;
    const Shores lasting = coast.shores();
    CHECK(mean_on_shore(coast, shores.volcanic, by_lava) > 0.3);
    // Basalt's pebbles still go along as shingle and are ground to dark sand where they lie.
    CHECK(mean_on_shore(coast, lasting.volcanic, along) > 0.05);
    CHECK(mean_on_shore(coast, shores.volcanic, along) <
          0.5 * mean_on_shore(coast, lasting.volcanic, along));
}

TEST_CASE("lava's dark sand breaks down to mud down long rivers", "[formation][shores]") {
    // Lava far inland, the rivers wearing it and bringing what's left to the coast.
    Coast coast{[](const DVec3& at) { return at.z < 0.0; }};
    coast.body.shell_sand = 0.0;
    coast.body.cliff_retreat = 0.0;
    for (std::uint32_t point = 0; point < coast.sphere.size(); ++point) {
        const DVec3 at = coast.sphere.point(point);
        if (!coast.in_sea(point)) {
            coast.rivers.wear[point] = 1e-4;
            coast.outcrop.rock[point] = at.z > 0.3 ? Rock::Lava : Rock::Basement;
        } else if (coast.on_shore(point)) {
            coast.sediment.settling[point] = 1.0e6;
        }
    }
    const Shores shores = coast.shores();
    coast.body.volcanic_reach = 0.0;
    const Shores lasting = coast.shores();
    const auto mouths = [](const DVec3& at) { return std::abs(at.z) < 0.1; };
    CHECK(mean_on_shore(coast, lasting.volcanic, mouths) > 0.2);
    CHECK(mean_on_shore(coast, shores.volcanic, mouths) <
          0.1 * mean_on_shore(coast, lasting.volcanic, mouths));
    // What broke down came as mud.
    CHECK(mean_on_shore(coast, shores.mud, mouths) >= mean_on_shore(coast, lasting.mud, mouths));
}

TEST_CASE("coral grows off warm, shallow, clear coasts, and its sand whitens their beaches",
          "[formation][shores]") {
    Coast coast{[](const DVec3& at) { return at.x < 0.0; }};
    for (std::uint32_t point = 0; point < coast.sphere.size(); ++point) {
        coast.climate.temperature[point] = 28.0 - (30.0 * std::abs(coast.sphere.point(point).z));
    }
    // A great river's mud pours out at the equator's coast.
    std::uint32_t muddy = 0;
    for (std::uint32_t point = 0; point < coast.sphere.size(); ++point) {
        const DVec3 at = coast.sphere.point(point);
        if (coast.on_shore(point) && std::abs(at.z) < 0.05 && at.y > 0.0) {
            muddy = point;
        }
    }
    REQUIRE(coast.on_shore(muddy));
    coast.sediment.settling[muddy] = 1.0e8;
    const Shores shores = coast.shores();
    const auto warm = [](const DVec3& at) { return std::abs(at.z) < 0.15 && at.y < 0.0; };
    const auto cold = [](const DVec3& at) { return std::abs(at.z) > 0.7; };
    CHECK(mean_on_shore(coast, shores.reef, warm) > 0.5);
    CHECK(mean_on_shore(coast, shores.reef, cold) == 0.0);
    CHECK(shores.reef[muddy] < 0.1);
    std::vector<double> carbonate(coast.sphere.size(), 0.0);
    for (std::uint32_t point = 0; point < coast.sphere.size(); ++point) {
        carbonate[point] = shores.carbonate[point] * shores.beach[point];
    }
    CHECK(mean_on_shore(coast, carbonate, warm) > 10.0 * mean_on_shore(coast, carbonate, cold));
}

TEST_CASE("what's brought to the shore is spread along it, all the sand lying somewhere",
          "[formation][shores]") {
    Coast coast{[](const DVec3& at) { return at.z < 0.0; }};
    coast.body.cliff_retreat = 0.0;
    coast.body.shell_sand = 0.0;
    coast.body.reef_sand = 0.0;
    std::uint32_t mouth = 0;
    for (std::uint32_t point = 0; point < coast.sphere.size(); ++point) {
        if (coast.on_shore(point) && coast.sphere.point(point).x > coast.sphere.point(mouth).x) {
            mouth = point;
        }
    }
    REQUIRE(coast.on_shore(mouth));
    constexpr double brought = 1.0e6;
    coast.sediment.settling[mouth] = brought;
    const Shores shores = coast.shores();
    double laid = 0.0;
    int beaches = 0;
    for (std::uint32_t point = 0; point < coast.sphere.size(); ++point) {
        if (coast.on_shore(point)) {
            laid += shores.beach[point] * coast.length();
            beaches += shores.beach[point] > 0.0 ? 1 : 0;
        }
    }
    CHECK_THAT(laid, WithinRel(brought * coast.body.rock_sand * coast.body.shore_years, 1e-6));
    CHECK(beaches > 2);
    CHECK(shores.quartz[mouth] == 1.0);
}

TEST_CASE("mud stays on sheltered shores, the open sea's waves wash it out",
          "[formation][shores]") {
    Coast coast{south_or_sound};
    coast.body.drift_reach = 1.0; // kept where it's brought
    std::uint32_t sheltered = 0;
    std::uint32_t open = 0;
    for (std::uint32_t point = 0; point < coast.sphere.size(); ++point) {
        const DVec3 at = coast.sphere.point(point);
        if (coast.on_shore(point) && angle(at, sound_at) < sound_radius) {
            sheltered = point;
        }
        if (coast.on_shore(point) && on_open_coast(at) && at.x < -0.5) {
            open = point;
        }
    }
    REQUIRE(coast.on_shore(sheltered));
    REQUIRE(coast.on_shore(open));
    coast.sediment.settling[sheltered] = 1.0e6;
    coast.sediment.settling[open] = 1.0e6;
    const Shores shores = coast.shores();
    CHECK(shores.mud[sheltered] > 1.5 * shores.mud[open]);
    CHECK(shores.mud[open] > 0.0);
}

TEST_CASE("a world's shores lie only along its waterline, what they're made of all told",
          "[formation][shores]") {
    static const World made{Body{}, 4, 6};
    const Shores& shores = made.shores;
    const auto in_sea = [&](std::uint32_t point) { return made.rivers.receiver[point] == point; };
    int beaches = 0;
    for (std::uint32_t point = 0; point < made.sphere.size(); ++point) {
        bool coast = false;
        for (const std::uint32_t near : made.sphere.neighbors(point)) {
            coast = coast || in_sea(near) != in_sea(point);
        }
        CHECK(shores.beach[point] >= 0.0);
        CHECK(shores.mud[point] >= 0.0);
        if (!coast) {
            CHECK(shores.beach[point] == 0.0);
            CHECK(shores.waves[point] == 0.0);
            continue;
        }
        if (shores.beach[point] > 0.0) {
            ++beaches;
            CHECK_THAT(shores.quartz[point] + shores.volcanic[point] + shores.carbonate[point] +
                           shores.shingle[point],
                       WithinAbs(1.0, 1e-9));
        }
    }
    CHECK(beaches > 100);
}

TEST_CASE("shores on a working world", "[.bench][formation][shores]") {
    static const World made{Body{}, 5, 7};
    const auto started = std::chrono::steady_clock::now();
    const Shores shores = shores_on(made.sphere, made.climate, made.rivers, made.bedrock,
                                    made.outcrop, made.ice, made.sediment, made.body);
    WARN("shores in "
         << std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count()
         << " s");
    int coast = 0;
    int beaches = 0;
    int dark = 0;
    int pale = 0;
    int stony = 0;
    int muddy = 0;
    int reefs = 0;
    double waves = 0.0;
    std::vector<double> volumes;
    for (std::uint32_t point = 0; point < made.sphere.size(); ++point) {
        if (made.rivers.receiver[point] != point || shores.waves[point] <= 0.0) {
            continue;
        }
        ++coast;
        waves += shores.waves[point];
        volumes.push_back(shores.beach[point]);
        beaches += shores.beach[point] > 1.0 ? 1 : 0;
        dark += shores.volcanic[point] > 0.5 ? 1 : 0;
        pale += shores.carbonate[point] > 0.5 ? 1 : 0;
        stony += shores.shingle[point] > 0.5 ? 1 : 0;
        muddy += shores.mud[point] > shores.beach[point] ? 1 : 0;
        reefs += shores.reef[point] > 0.5 ? 1 : 0;
    }
    std::array<int, 5> rocks{};
    for (std::uint32_t point = 0; point < made.sphere.size(); ++point) {
        if (made.rivers.receiver[point] != point && shores.waves[point] > 0.0) {
            ++rocks[static_cast<std::size_t>(made.outcrop.rock[point])];
        }
    }
    WARN("coast rock: lava " << rocks[0] << ", layered " << rocks[1] << ", folded " << rocks[2]
                             << ", basement " << rocks[3] << ", sea floor " << rocks[4]);
    std::ranges::sort(volumes);
    const auto at = [&](double share) {
        return volumes[static_cast<std::size_t>(share * static_cast<double>(volumes.size() - 1))];
    };
    WARN("coast " << coast << ", waves " << waves / coast << " m, beach m3/m 10% " << at(0.1)
                  << " 50% " << at(0.5) << " 90% " << at(0.9) << "; beaches " << beaches
                  << ", dark " << dark << ", pale " << pale << ", stony " << stony << ", muddy "
                  << muddy << ", reefs " << reefs);
}
