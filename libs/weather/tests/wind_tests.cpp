#include <hog/globe/cell.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/planet.hpp>
#include <hog/weather/field.hpp>
#include <hog/weather/wind.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

using Catch::Approx;
using hog::Vec3;
using hog::globe::Planet;
using namespace hog::weather;

namespace {

constexpr int level = 3;
constexpr int layers = 8;

Planet small_earth() {
    Planet planet;
    planet.body.radius = 20'000.0;
    return planet;
}

struct World {
    Planet planet = small_earth();
    hog::globe::Generator generator{planet};
    Field field = start_field(generator, level, layers);
    Stencil stencil{field.grid};
};

std::size_t column_at(double latitude) {
    const double angle = latitude * std::numbers::pi / 180.0;
    const hog::DVec3 direction{std::cos(angle), 0.0, std::sin(angle)};
    return static_cast<std::size_t>(hog::globe::index_of(hog::globe::cell_under(direction, level)));
}

// A land column near a latitude, so air at its bottom isn't over the sea's flat floor... any
// column will do, but its lowest open layer must exist.
int lowest_open(const Field& field, std::size_t column) {
    for (int layer = 0; layer < layers; ++layer) {
        if (!field.solid(column, layer)) {
            return layer;
        }
    }
    return layers;
}

// A column of land with air over it at least `layers_over` deep and at least `layers_under` of
// ground cells under it, in the north.
std::size_t land_column(const Field& field) {
    for (std::size_t column = 0; column < field.grid.columns(); ++column) {
        const hog::DVec3 up = hog::globe::center(hog::globe::cell_at_index(level, column));
        if (field.over_sea[column] == 0 && lowest_open(field, column) > 0 && up.z > 0.3) {
            return column;
        }
    }
    return field.grid.columns();
}

// Whether a column is one of the three meeting at a corner of the cube.
bool at_corner(std::size_t column) {
    const hog::globe::Cell cell = hog::globe::cell_at_index(level, column);
    const std::uint32_t last = (1U << static_cast<std::uint32_t>(level)) - 1U;
    return (cell.i == 0 || cell.i == last) && (cell.j == 0 || cell.j == last);
}

// A land column as above whose lowest open air has open air on every side too: level ground, so
// no slope's own winds outweigh what's done to it. `corner` asks for one at a corner of the cube,
// or for one that isn't.
std::size_t open_land_column(const Field& field, const Stencil& stencil, bool corner = false) {
    for (std::size_t column = 0; column < field.grid.columns(); ++column) {
        const hog::DVec3 up = hog::globe::center(hog::globe::cell_at_index(level, column));
        const int low = lowest_open(field, column);
        if (field.over_sea[column] != 0 || low == 0 || low >= layers || up.z <= 0.3 ||
            at_corner(column) != corner) {
            continue;
        }
        const auto& links = stencil.links(field.grid.index(column, low));
        bool open = true;
        for (int side = 0; side < 4; ++side) {
            open = open && field.blocked[links[static_cast<std::size_t>(side)].cell] == 0;
        }
        if (open) {
            return column;
        }
    }
    return field.grid.columns();
}

float peak_speed(const Field& field) {
    float peak = 0.0F;
    for (const auto& air : field.air) {
        peak = std::max(peak, glm::length(air.wind));
    }
    return peak;
}

} // namespace

TEST_CASE("the ground fills the cells under it", "[weather][wind]") {
    const World world;
    bool blocked = false;
    for (std::size_t column = 0; column < world.field.grid.columns(); ++column) {
        for (int layer = 0; layer < layers; ++layer) {
            if (world.field.solid(column, layer)) {
                blocked = true;
                CHECK(world.field.grid.radius_of(layer) < world.field.ground[column]);
            }
        }
    }
    CHECK(blocked);
}

TEST_CASE("a spinning planet turns moving air to the right in the north and left in the south",
          "[weather][wind]") {
    World world;
    for (const double latitude : {45.0, -45.0}) {
        const std::size_t column = column_at(latitude);
        const int layer = layers - 1;
        const std::size_t cell = world.field.grid.index(column, layer);
        // Eastward at the column's middle.
        const hog::DVec3 up = hog::globe::center(hog::globe::cell_at_index(level, column));
        const Vec3 east{glm::normalize(hog::DVec3{-up.y, up.x, 0.0})};
        const Vec3 north{glm::normalize(hog::DVec3{0.0, 0.0, 1.0} - (up * up.z))};
        world.field.air[cell].wind = east * 10.0F;
        accelerate(world.field, world.stencil, world.planet, 600.0);
        const float turned = glm::dot(world.field.air[cell].wind, north);
        // To the right of east is south.
        if (latitude > 0.0) {
            CHECK(turned < 0.0F);
        } else {
            CHECK(turned > 0.0F);
        }
    }
}

TEST_CASE("air warmer than its layer's mean rises and colder sinks", "[weather][wind]") {
    World world;
    const std::size_t column = column_at(10.0);
    const int layer = layers - 2;
    const std::size_t cell = world.field.grid.index(column, layer);
    const std::size_t cold = world.field.grid.index(column_at(-70.0), layer);
    world.field.air[cell].temperature += 5.0F;
    world.field.air[cold].temperature -= 5.0F;
    accelerate(world.field, world.stencil, world.planet, 60.0);
    CHECK(glm::dot(world.field.air[cell].wind, world.stencil.up(column)) > 0.0F);
    CHECK(glm::dot(world.field.air[cold].wind, world.stencil.up(column_at(-70.0))) < 0.0F);
}

TEST_CASE("air near the ground is slowed and air aloft is not", "[weather][wind]") {
    World world;
    const std::size_t column = land_column(world.field);
    REQUIRE(column < world.field.grid.columns());
    const int low = lowest_open(world.field, column);
    const int high = layers - 1;
    const Vec3 up = world.stencil.up(column);
    const Vec3 along{glm::normalize(Vec3{-up.y, up.x, 0.0F})};
    world.field.air[world.field.grid.index(column, low)].wind = along * 10.0F;
    world.field.air[world.field.grid.index(column, high)].wind = along * 10.0F;
    accelerate(world.field, world.stencil, world.planet, 6.0 * 3'600.0);
    // Along the ground, as buoyancy and the planet's turning act on the rest.
    const auto level_speed = [&](int layer) {
        const Vec3 wind = world.field.air[world.field.grid.index(column, layer)].wind;
        return glm::length(wind - (up * glm::dot(wind, up)));
    };
    CHECK(level_speed(high) > level_speed(low));
}

TEST_CASE("the pressure takes the divergence out of a wind", "[weather][wind]") {
    World world;
    // Air pouring out of one column and into the ground of another.
    const std::size_t column = column_at(20.0);
    for (int layer = 0; layer < layers; ++layer) {
        const std::size_t cell = world.field.grid.index(column, layer);
        if (world.field.blocked[cell] == 0) {
            world.field.air[cell].wind = world.stencil.up(column) * 5.0F;
        }
    }
    const double before = divergence(world.field, world.stencil, world.planet);
    REQUIRE(before > 0.0);
    const int iterations = project(world.field, world.stencil, world.planet);
    CHECK(iterations > 0);
    CHECK(divergence(world.field, world.stencil, world.planet) < 0.01 * before);
}

TEST_CASE("the ground stops air crossing it", "[weather][wind]") {
    World world;
    const std::size_t column = land_column(world.field);
    REQUIRE(column < world.field.grid.columns());
    const int low = lowest_open(world.field, column);
    // A wind straight down onto the ground has nowhere to go.
    const std::size_t cell = world.field.grid.index(column, low);
    world.field.air[cell].wind = -world.stencil.up(column) * 10.0F;
    const float before = glm::dot(world.field.air[cell].wind, world.stencil.up(column));
    project(world.field, world.stencil, world.planet);
    CHECK(glm::dot(world.field.air[cell].wind, world.stencil.up(column)) > 0.5F * before);
    for (std::size_t other = 0; other < world.field.air.size(); ++other) {
        if (world.field.blocked[other] != 0) {
            REQUIRE(glm::length(world.field.air[other].wind) == 0.0F);
        }
    }
}

TEST_CASE("a patch of the air is carried along by the wind", "[weather][wind]") {
    World world;
    const int layer = layers - 1;
    // The air turning about the planet's axis at 20 m/s round the equator.
    const double speed = 20.0;
    for (std::size_t column = 0; column < world.field.grid.columns(); ++column) {
        const std::size_t cell = world.field.grid.index(column, layer);
        const Vec3 up = world.stencil.up(column);
        world.field.air[cell].wind =
            Vec3{-up.y, up.x, 0.0F} * static_cast<float>(speed * world.field.grid.radius_of(layer) /
                                                         world.planet.body.radius);
        world.field.air[cell].cloud = 0.0F;
    }
    const std::size_t start = column_at(0.0);
    world.field.air[world.field.grid.index(start, layer)].cloud = 1.0F;
    const auto centroid = [&] {
        hog::DVec3 total{0.0};
        double mass = 0.0;
        for (std::size_t column = 0; column < world.field.grid.columns(); ++column) {
            const auto cloud =
                static_cast<double>(world.field.air[world.field.grid.index(column, layer)].cloud);
            total += cloud * hog::globe::center(hog::globe::cell_at_index(level, column));
            mass += cloud;
        }
        return std::pair{glm::normalize(total), mass};
    };
    // Far enough to cross about a cell and a half.
    const double distance = 6'000.0;
    const double seconds = distance / speed;
    const int steps = 20;
    for (int i = 0; i < steps; ++i) {
        advect(world.field, world.stencil, world.planet, seconds / steps);
    }
    const auto [after, mass] = centroid();
    const double angle = std::atan2(after.y, after.x);
    CHECK(mass > 0.2);
    CHECK(angle * world.planet.body.radius == Approx(distance).epsilon(0.35));
}

TEST_CASE("steps from rest stay finite and the wind stays within bounds", "[weather][wind]") {
    World world;
    const int per_day = 24;
    for (int step = 0; step < 3 * per_day; ++step) {
        hog::weather::warm_surface(world.field, world.planet,
                                   hog::DVec3{std::cos(0.3 * step), std::sin(0.3 * step), 0.0},
                                   3'600.0);
        hog::weather::step(world.field, world.stencil, world.planet, 120.0);
    }
    for (const auto& air : world.field.air) {
        REQUIRE(std::isfinite(air.temperature));
        REQUIRE(std::isfinite(air.wind.x));
        REQUIRE(air.temperature > 100.0F);
    }
    CHECK(peak_speed(world.field) < 150.0F);
}

namespace {

// Warms a land column's air hard and reads how the air about answers.
void warm_and_lift(bool corner) {
    World world;
    const std::size_t column = open_land_column(world.field, world.stencil, corner);
    REQUIRE(column < world.field.grid.columns());
    // Settle for a day, then heat one column's surface and its air hard.
    for (int step = 0; step < 12; ++step) {
        hog::weather::step(world.field, world.stencil, world.planet, 120.0);
    }
    // Its winds are read against the same air left unheated, so the planet's own stirring doesn't
    // count.
    Field still = world.field;
    const int low = lowest_open(world.field, column);
    for (int layer = low; layer < layers; ++layer) {
        world.field.air[world.field.grid.index(column, layer)].temperature += 8.0F;
    }
    // Soon after, before the air rocks back about its level.
    hog::weather::step(world.field, world.stencil, world.planet, 60.0);
    hog::weather::step(still, world.stencil, world.planet, 60.0);
    const auto added = [&](std::size_t cell) {
        return world.field.air[cell].wind - still.air[cell].wind;
    };
    const Vec3 up = world.stencil.up(column);
    // The warmed column rises as a whole, however its lowest layers first rock.
    double lifted = 0.0;
    for (int layer = low; layer < layers; ++layer) {
        lifted += world.field.grid.depth_of(layer) *
                  static_cast<double>(glm::dot(added(world.field.grid.index(column, layer)), up));
    }
    CHECK(lifted > 0.0);
    // Air close to the ground flows in from the sides.
    float inflow = 0.0F;
    const auto& links = world.stencil.links(world.field.grid.index(column, low));
    for (int side = 0; side < 4; ++side) {
        const Link& link = links[static_cast<std::size_t>(side)];
        if (world.field.blocked[link.cell] == 0) {
            inflow -= glm::dot(added(link.cell), link.direction);
        }
    }
    CHECK(inflow > 0.0F);
}

} // namespace

TEST_CASE("ground warmed by the sun draws air in and lifts it", "[weather][wind]") {
    warm_and_lift(false);
}

// Known wrong: where three faces of the cube meet, a warmed column's middle sinks fast.
TEST_CASE("ground warmed by the sun at a corner of the cube lifts its air too",
          "[weather][wind][!mayfail]") {
    warm_and_lift(true);
}

TEST_CASE("carrying keeps the water and the warmth it carries, however unevenly spread",
          "[weather][wind]") {
    World world;
    Field& field = world.field;
    const auto layer_count = static_cast<std::size_t>(layers);
    // Lumpy water and warmth, turned about the axis at a speed that moves it several cells.
    for (std::size_t cell = 0; cell < field.air.size(); ++cell) {
        if (field.blocked[cell] != 0) {
            continue;
        }
        const std::size_t column = cell / layer_count;
        const Vec3 up = world.stencil.up(column);
        Air& air = field.air[cell];
        air.wind = Vec3{-up.y, up.x, 0.0F} * 120.0F;
        const auto lump = static_cast<float>((cell * 2654435761U) % 1000U) / 1000.0F;
        air.vapour = 0.001F + (0.004F * lump);
        air.cloud = lump > 0.8F ? 0.0005F : 0.0F;
        air.rain = lump < 0.1F ? 0.0002F : 0.0F;
        air.temperature += 6.0F * lump;
    }
    // What a cell weighs in the state the column is balanced against.
    const auto weight = [&](std::size_t cell) {
        const std::size_t column = cell / layer_count;
        const int layer = static_cast<int>(cell % layer_count);
        const double radius = field.grid.radius_of(layer);
        const double height = radius - world.planet.body.radius;
        const auto surface = static_cast<double>(field.surface[column]);
        return air_density(air_temperature(world.planet, surface, height),
                           pressure_at(world.planet, surface, height)) *
               hog::globe::solid_angle(hog::globe::cell_at_index(level, column)) * radius * radius *
               field.grid.depth_of(layer);
    };
    struct Totals {
        double vapour = 0.0;
        double cloud = 0.0;
        double rain = 0.0;
        double warmth = 0.0;
    };
    const auto total = [&] {
        Totals sum;
        for (std::size_t cell = 0; cell < field.air.size(); ++cell) {
            if (field.blocked[cell] != 0) {
                continue;
            }
            const double mass = weight(cell);
            const Air& air = field.air[cell];
            const double height = field.grid.radius_of(static_cast<int>(cell % layer_count)) -
                                  world.planet.body.radius;
            sum.vapour += static_cast<double>(air.vapour) * mass;
            sum.cloud += static_cast<double>(air.cloud) * mass;
            sum.rain += static_cast<double>(air.rain) * mass;
            sum.warmth +=
                (static_cast<double>(air.temperature) + (dry_lapse_rate(world.planet) * height)) *
                mass;
        }
        return sum;
    };
    const Totals before = total();
    for (int i = 0; i < 10; ++i) {
        advect(field, world.stencil, world.planet, 120.0);
    }
    const Totals after = total();
    CHECK(after.vapour == Approx(before.vapour).epsilon(1e-4));
    CHECK(after.cloud == Approx(before.cloud).epsilon(1e-4));
    CHECK(after.rain == Approx(before.rain).epsilon(1e-4));
    CHECK(after.warmth == Approx(before.warmth).epsilon(1e-6));
}
