#include "fine_planet.hpp"

#include <hog/globe/brick.hpp>
#include <hog/globe/cell.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/held_tiles.hpp>
#include <hog/globe/planet.hpp>
#include <hog/globe/volume_build.hpp>
#include <hog/globe/voxel.hpp>
#include <hog/globe/world.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numbers>
#include <optional>
#include <set>
#include <span>
#include <utility>
#include <vector>

using hog::DVec3;
using hog::globe::BrickSite;
using hog::globe::BrickVoxels;
using hog::globe::Generator;
using hog::globe::MadeWorld;
using hog::globe::Material;
using hog::globe::Site;
using hog::globe::Voxel;
using hog::globe::World;

namespace {

const Generator& earth() {
    static const Generator generator{fine(hog::globe::Planet{})};
    return generator;
}

// The brick the ground passes through above a direction, and the ground's radius there.
struct Ground {
    BrickSite brick;
    double radius = 0.0;
};

Ground ground_at(const Generator& generator, DVec3 direction) {
    const double radius = hog::globe::ground_below(generator, direction, 0.5).value_or(0.0);
    const hog::globe::Layers& layers = generator.layers();
    const hog::globe::Cell cell = hog::globe::cell_under(direction, layers.level);
    const Site site{.face = cell.face,
                    .i = cell.i,
                    .j = cell.j,
                    .layer = static_cast<std::uint32_t>(layers.layer_at(radius))};
    return {.brick = hog::globe::brick_of(site), .radius = radius};
}

DVec3 somewhere(std::uint64_t k) {
    return hog::globe::center(hog::globe::cell_at_index(7, (k * 2'654'435'761ULL) % 98'304));
}

// The true distance from a point over a column to the ground, by the nearest of the ground's
// points around it.
double true_distance(const Generator& generator, DVec3 direction, double radius, double around,
                     double lod = 0.5) {
    const DVec3 axis = std::abs(direction.z) < 0.9 ? DVec3{0.0, 0.0, 1.0} : DVec3{1.0, 0.0, 0.0};
    const DVec3 east = glm::normalize(glm::cross(axis, direction));
    const DVec3 north = glm::cross(direction, east);
    const DVec3 point = direction * radius;
    constexpr int steps = 8;
    double nearest = 1e9;
    for (int a = -steps; a <= steps; ++a) {
        for (int b = -steps; b <= steps; ++b) {
            const double x = around * a / steps;
            const double y = around * b / steps;
            const DVec3 there = glm::normalize(direction + ((x * east + y * north) / radius));
            if (const auto ground = hog::globe::ground_below(generator, there, lod)) {
                nearest = std::min(nearest, glm::length(there * *ground - point));
            }
        }
    }
    return nearest;
}

} // namespace

TEST_CASE("a cell's column comes out the same in any square of them", "[globe][world]") {
    const Generator& generator = earth();
    const int level = generator.layers().level;
    const auto side = std::uint32_t{1} << level;
    // In a face's middle, and in its corner where three faces meet.
    for (const hog::globe::Cell corner :
         {hog::globe::Cell{
              .level = static_cast<std::uint8_t>(level), .face = 2, .i = 5'000, .j = 7'000},
          hog::globe::Cell{.level = static_cast<std::uint8_t>(level),
                           .face = 4,
                           .i = side - 3,
                           .j = side - 3}}) {
        const std::vector<Generator::Column> square = generator.columns(corner, 3, 0.5);
        for (std::uint32_t k = 0; k < 9; ++k) {
            const hog::globe::Cell cell{.level = corner.level,
                                        .face = corner.face,
                                        .i = corner.i + (k % 3),
                                        .j = corner.j + (k / 3)};
            const Generator::Column alone = generator.columns(cell, 1, 0.5)[0];
            for (const double radius : {114'000.0, 115'000.0, 116'000.0}) {
                const hog::globe::Sample a = square[k].sample(radius);
                const hog::globe::Sample b = alone.sample(radius);
                CHECK(a.distance == b.distance);
                CHECK(a.material == b.material);
            }
        }
    }
}

TEST_CASE("slopes between neighbouring columns tell distance as truly as small steps",
          "[globe][world]") {
    const Generator& generator = earth();
    const hog::globe::Layers& layers = generator.layers();
    double shared_error = 0.0;
    double stepped_error = 0.0;
    int counted = 0;
    for (std::uint64_t k = 0; k < 16; ++k) {
        const hog::globe::Cell cell = hog::globe::cell_under(somewhere(k), layers.level);
        const DVec3 direction = hog::globe::center(cell);
        const double ground = hog::globe::ground_below(generator, direction, 0.5).value_or(0.0);
        const Generator::Column shared = generator.columns(cell, 1, 0.5)[0];
        const Generator::Column stepped = generator.column(direction, 0.5);
        for (const double height : {0.5, 1.0, 2.0}) {
            const double truth = true_distance(generator, direction, ground + height, 2.0 * height);
            const double a = std::abs(shared.sample(ground + height).distance - truth) / truth;
            const double b = std::abs(stepped.sample(ground + height).distance - truth) / truth;
            shared_error += a;
            stepped_error += b;
            ++counted;
        }
    }
    // Both are first-order: they miss by the ground's curvature, a few percent on average.
    CHECK(shared_error <= stepped_error * 1.2);
    CHECK(shared_error / counted < 0.08);
}

TEST_CASE("a world is its volume's voxels, air wherever nothing is made", "[globe][world]") {
    MadeWorld made{earth()};
    const World& world = made.world();
    // Somewhere the ground passes through the middle layers of its brick.
    std::uint64_t k = 11;
    const auto layer_in_brick = [&](DVec3 at) {
        return earth().layers().layer_at(ground_at(earth(), at).radius) % hog::globe::brick_side;
    };
    while (layer_in_brick(somewhere(k)) < 2 || layer_in_brick(somewhere(k)) > 5) {
        ++k;
    }
    const DVec3 direction = somewhere(k);
    const BrickSite brick = ground_at(earth(), direction).brick;
    // Nothing made yet: air through and through.
    for (const Voxel& voxel : world.brick(brick)) {
        CHECK(voxel.solid == Material::Air);
    }
    made.cover(direction, 1);
    const BrickVoxels voxels = world.brick(brick);
    int inside = 0;
    for (int index = 0; index < hog::globe::brick_voxels; index += 7) {
        const Voxel voxel = voxels[static_cast<std::size_t>(index)];
        const Site site = hog::globe::site_in(brick, index);
        CHECK(world.at(site) == voxel);
        CHECK(voxel == hog::globe::voxel_of(made.volume().at(site),
                                            hog::globe::place_temperature(earth().planet(),
                                                                          earth().layers(), site)));
        inside += voxel.solid_amount > Voxel::full / 2 ? 1 : 0;
    }
    // The ground passes through it.
    CHECK(inside > 0);
    CHECK(inside < 74);
}

TEST_CASE("a brick written as it is changes nothing", "[globe][world]") {
    MadeWorld made{earth()};
    World& world = made.world();
    const DVec3 direction = somewhere(3);
    made.cover(direction, 1);
    const BrickSite brick = ground_at(earth(), direction).brick;
    const std::uint64_t before = world.revision();
    world.write(brick, world.brick(brick));
    CHECK(world.revision() == before);
    CHECK(made.volume().unsaved().empty());
    BrickVoxels voxels = world.brick(brick);
    voxels[5].condition = 100;
    world.write(brick, voxels);
    CHECK(world.changed_since(before) == std::vector{brick});
    CHECK(world.brick(brick)[5].condition == 100);
    CHECK(made.volume().unsaved().size() == 1);
}

TEST_CASE("the sites near a point are every column in reach and none a cell past it",
          "[globe][world]") {
    const hog::globe::Layers& layers = earth().layers();
    // Well inside a face, so every column in reach is on it.
    const DVec3 direction = glm::normalize(DVec3{1.0, 0.3, -0.2});
    const double from = layers.middle(layers.count / 2);
    const DVec3 centre = direction * from;
    const double reach = 3.0 * layers.thickness;
    const double cell = std::numbers::pi / 2.0 * from / std::ldexp(1.0, layers.level);

    std::set<std::pair<std::uint32_t, std::uint32_t>> found;
    for (const auto& [brick, indices] : hog::globe::sites_near(layers, centre, reach)) {
        for (const int index : indices) {
            const Site site = hog::globe::site_in(brick, index);
            CHECK(site.face == hog::globe::cell_under(direction, layers.level).face);
            found.emplace(site.i, site.j);
        }
    }
    const hog::globe::Cell under = hog::globe::cell_under(direction, layers.level);
    const auto span = static_cast<int>(std::ceil(reach / cell)) + 3;
    for (int di = -span; di <= span; ++di) {
        for (int dj = -span; dj <= span; ++dj) {
            const hog::globe::Cell column{
                .level = under.level,
                .face = under.face,
                .i = static_cast<std::uint32_t>(static_cast<int>(under.i) + di),
                .j = static_cast<std::uint32_t>(static_cast<int>(under.j) + dj)};
            const double apart = glm::length((hog::globe::center(column) * from) - centre);
            const bool near = found.contains({column.i, column.j});
            if (apart <= reach) {
                CHECK(near);
            }
            if (apart > reach + cell) {
                CHECK_FALSE(near);
            }
        }
    }
}

TEST_CASE("a ball given as a shape digs and builds as a ball does", "[globe][world]") {
    const DVec3 direction = somewhere(29);
    const DVec3 centre = direction * ground_at(earth(), direction).radius;
    const hog::globe::Shape ball = [&](DVec3 at) { return glm::length(at - centre) - 3.0; };
    MadeWorld by_ball{earth()};
    MadeWorld by_shape{earth()};
    by_ball.cover(direction);
    by_shape.cover(direction);
    World& ball_world = by_ball.world();
    World& shape_world = by_shape.world();
    hog::globe::dig(ball_world, centre, 3.0);
    hog::globe::dig_shape(shape_world, centre, 3.0, 3.0, ball);
    hog::globe::build(ball_world, centre, 2.0, Material::Granite);
    hog::globe::build_shape(
        shape_world, centre, 2.0, 2.0, [&](DVec3 at) { return glm::length(at - centre) - 2.0; },
        Material::Granite);
    CHECK(shape_world.changed_since(0) == ball_world.changed_since(0));
    const hog::globe::Cell cell = hog::globe::cell_under(direction, earth().layers().level);
    for (int layer = -6; layer <= 6; ++layer) {
        const Site site{.face = cell.face,
                        .i = cell.i,
                        .j = cell.j,
                        .layer = static_cast<std::uint32_t>(
                            earth().layers().layer_at(glm::length(centre)) + layer)};
        CHECK(shape_world.at(site) == ball_world.at(site));
    }
}

TEST_CASE("digging opens a ball in the ground and building fills one", "[globe][world]") {
    MadeWorld made{earth()};
    World& world = made.world();
    const DVec3 direction = somewhere(29);
    made.cover(direction);
    const Ground ground = ground_at(earth(), direction);
    const hog::globe::Layers& layers = earth().layers();
    const DVec3 centre = direction * ground.radius;
    const hog::globe::Cell cell = hog::globe::cell_under(direction, layers.level);
    const auto site_at = [&](double radius) {
        return Site{.face = cell.face,
                    .i = cell.i,
                    .j = cell.j,
                    .layer = static_cast<std::uint32_t>(layers.layer_at(radius))};
    };

    hog::globe::dig(world, centre, 3.0);
    // Its middle and its floor are open, the rock under the floor is not.
    CHECK(world.at(site_at(ground.radius)).solid_amount < Voxel::full / 2);
    CHECK(world.at(site_at(ground.radius)).solid == Material::Air);
    CHECK(world.at(site_at(ground.radius - 2.5)).solid_amount < Voxel::full / 2);
    CHECK(world.at(site_at(ground.radius - 3.5)).solid_amount > Voxel::full / 2);
    const std::size_t dug = world.changed_since(0).size();
    // Two bricks or more each way, and some more above and below it.
    CHECK(dug >= 4);
    CHECK(dug < 60);

    hog::globe::build(world, centre, 2.0, Material::Granite);
    CHECK(world.at(site_at(ground.radius)).solid_amount > Voxel::full / 2);
    CHECK(world.at(site_at(ground.radius)).solid == Material::Granite);
    CHECK(world.at(site_at(ground.radius - 2.6)).solid_amount < Voxel::full / 2);

    // A ball dug out of the open sky changes nothing.
    const std::uint64_t built = world.revision();
    hog::globe::dig(world, direction * (ground.radius + 200.0), 3.0);
    CHECK(world.revision() == built);
}

TEST_CASE("a ball dug across a cube's edge opens on both faces", "[globe][world]") {
    MadeWorld made{earth()};
    World& world = made.world();
    const DVec3 edge = glm::normalize(DVec3{1.0, 1.0, 0.3});
    made.cover(glm::normalize(edge + DVec3{1e-5, 0.0, 0.0}));
    made.cover(glm::normalize(edge + DVec3{0.0, 1e-5, 0.0}));
    const Ground ground = ground_at(earth(), edge);
    hog::globe::dig(world, edge * ground.radius, 4.0);
    std::set<int> faces;
    for (const BrickSite& site : world.changed_since(0)) {
        faces.insert(site.face);
    }
    CHECK(faces == std::set<int>{0, 1});
}

TEST_CASE("columns far apart tell distance at their coarser detail as truly as small steps",
          "[globe][world]") {
    const Generator& generator = earth();
    const hog::globe::Layers& layers = generator.layers();
    constexpr int apart = 16;
    const double lod = hog::globe::voxel_lod(generator) * apart;
    double shared_error = 0.0;
    double stepped_error = 0.0;
    int counted = 0;
    for (std::uint64_t k = 0; k < 16; ++k) {
        const hog::globe::Cell cell = hog::globe::cell_under(somewhere(k), layers.level);
        const DVec3 direction = hog::globe::center(cell);
        const double ground = hog::globe::ground_below(generator, direction, lod).value_or(0.0);
        const Generator::Column shared = generator.columns(std::span{&cell, 1}, lod, apart)[0];
        const Generator::Column stepped = generator.column(direction, lod);
        for (const double height : {8.0, 16.0, 32.0}) {
            const double truth =
                true_distance(generator, direction, ground + height, 2.0 * height, lod);
            shared_error += std::abs(shared.sample(ground + height).distance - truth) / truth;
            stepped_error += std::abs(stepped.sample(ground + height).distance - truth) / truth;
            ++counted;
        }
    }
    CHECK(shared_error <= stepped_error * 1.2);
    CHECK(shared_error / counted < 0.08);
}

TEST_CASE("voxels filled far apart are the world's, averaged through the layers they stand for",
          "[globe][world]") {
    MadeWorld made{earth()};
    World& world = made.world();
    const hog::globe::Layers& layers = earth().layers();
    const DVec3 direction = glm::normalize(somewhere(3));
    made.cover(direction);
    const Ground ground = ground_at(earth(), direction);
    hog::globe::dig(world, direction * ground.radius, 6.0);
    constexpr int apart = 4;
    const hog::globe::Cell middle = hog::globe::cell_under(direction, layers.level);
    std::vector<hog::globe::Cell> cells;
    for (int b = -4; b <= 4; ++b) {
        for (int a = -4; a <= 4; ++a) {
            cells.push_back(hog::globe::next_to(middle, a * apart, b * apart).value_or(middle));
        }
    }
    const int bottom = layers.layer_at(ground.radius) - 40;
    constexpr int count = 20;
    const std::vector<Voxel> voxels = world.fill(cells, bottom, count, apart);
    int inside = 0;
    for (std::size_t k = 0; k < cells.size(); ++k) {
        for (int step = 0; step < count; ++step) {
            const Site site{.face = cells[k].face,
                            .i = cells[k].i,
                            .j = cells[k].j,
                            .layer = static_cast<std::uint32_t>(bottom + (step * apart))};
            const Voxel voxel =
                voxels[(k * static_cast<std::size_t>(count)) + static_cast<std::size_t>(step)];
            std::uint32_t solid = 0;
            std::uint32_t liquid = 0;
            for (std::uint32_t up = 0; up < apart; ++up) {
                Site layer = site;
                layer.layer += up;
                solid += world.at(layer).solid_amount;
                liquid += world.at(layer).liquid_amount;
            }
            CHECK(voxel.solid_amount == (solid + (apart / 2)) / apart);
            CHECK(voxel.liquid_amount == (liquid + (apart / 2)) / apart);
            CHECK(voxel.temperature == world.at(site).temperature);
            inside += voxel.solid != Material::Air ? 1 : 0;
        }
    }
    CHECK(inside > 0);
}

TEST_CASE("columns filled together are the world voxel by voxel, dug or not", "[globe][world]") {
    MadeWorld made{earth()};
    World& world = made.world();
    const hog::globe::Layers& layers = earth().layers();
    const DVec3 edge = glm::normalize(DVec3{1.0, 1.0, 0.3});
    made.cover(glm::normalize(edge + DVec3{1e-5, 0.0, 0.0}));
    made.cover(glm::normalize(edge + DVec3{0.0, 1e-5, 0.0}));
    const Ground ground = ground_at(earth(), edge);
    hog::globe::dig(world, edge * ground.radius, 2.0);
    const std::vector<BrickSite> changed = world.changed_since(0);
    // A row of cells across the cube's edge, through the hole.
    const hog::globe::Cell middle = hog::globe::cell_under(edge, layers.level);
    std::vector<hog::globe::Cell> cells{middle};
    for (int step = 0; step < 6; ++step) {
        const auto next = hog::globe::next_to(cells.back(), 1, 0);
        REQUIRE(next);
        cells.push_back(next.value_or(middle));
    }
    const int bottom = layers.layer_at(ground.radius) - 12;
    constexpr int count = 24;
    const std::vector<Voxel> voxels = world.fill(cells, bottom, count);
    const auto across = static_cast<std::size_t>(count);
    REQUIRE(voxels.size() == cells.size() * across);
    std::set<int> faces;
    bool dug = false;
    for (std::size_t k = 0; k < cells.size(); ++k) {
        faces.insert(cells[k].face);
        for (int layer = 0; layer < count; ++layer) {
            const Site site{.face = cells[k].face,
                            .i = cells[k].i,
                            .j = cells[k].j,
                            .layer = static_cast<std::uint32_t>(bottom + layer)};
            const Voxel voxel = voxels[(k * across) + static_cast<std::size_t>(layer)];
            CHECK(voxel == world.at(site));
            dug = dug || (std::ranges::binary_search(changed, hog::globe::brick_of(site)) &&
                          voxel.solid_amount < Voxel::full / 2 &&
                          glm::length(middle_of(site, layers)) < ground.radius);
        }
    }
    CHECK(faces.size() == 2);
    CHECK(dug);
}

TEST_CASE("bricks fill from the generator", "[.bench][globe][world]") {
    {
        const Generator generator{hog::globe::Planet{}};
        std::vector<BrickSite> bricks;
        bricks.reserve(40);
        for (std::uint64_t k = 0; k < 40; ++k) {
            bricks.push_back(ground_at(generator, somewhere(k)).brick);
        }
        const auto start = std::chrono::steady_clock::now();
        std::size_t inside = 0;
        for (const BrickSite& brick : bricks) {
            for (const Voxel& voxel : hog::globe::generated(generator, brick)) {
                inside += voxel.solid_amount > Voxel::full / 2 ? 1U : 0U;
            }
        }
        const std::chrono::duration<double> took = std::chrono::steady_clock::now() - start;
        // The same columns each measuring its own slope, for comparison.
        const auto alone_start = std::chrono::steady_clock::now();
        for (const BrickSite& brick : bricks) {
            for (int k = 0; k < hog::globe::brick_side * hog::globe::brick_side; ++k) {
                const Site site = hog::globe::site_in(brick, k);
                const Generator::Column column = generator.column(
                    hog::globe::center(hog::globe::column_of(site, generator.layers())),
                    hog::globe::voxel_lod(generator));
                const double distance =
                    column.sample(generator.layers().middle(static_cast<int>(site.layer))).distance;
                inside += distance < 0.0 ? 1U : 0U;
            }
        }
        const std::chrono::duration<double> alone = std::chrono::steady_clock::now() - alone_start;
        WARN("bricks: " << took.count() * 1e3 / static_cast<double>(bricks.size())
                        << " ms a brick, "
                        << alone.count() * 1e3 / static_cast<double>(bricks.size())
                        << " ms with slopes of their own (" << inside << " inside)");
    }
}

TEST_CASE("a voxel dug or built is matter just where it's more than half full",
          "[globe][world][amounts]") {
    MadeWorld made{earth()};
    World& world = made.world();
    const DVec3 direction = somewhere(11);
    made.cover(direction);
    const Ground ground = ground_at(earth(), direction);
    hog::globe::dig(world, direction * ground.radius, 3.0);
    hog::globe::build(world, direction * (ground.radius - 1.0), 1.5, Material::Granite);
    std::size_t checked = 0;
    for (const BrickSite site : world.changed_since(0)) {
        for (const Voxel& voxel : world.brick(site)) {
            // Exactly half lies on the surface, either way.
            if (voxel.solid_amount != Voxel::full / 2) {
                CHECK((voxel.solid != Material::Air) == (voxel.solid_amount > Voxel::full / 2));
                ++checked;
            }
        }
    }
    CHECK(checked > 1'000);
}

TEST_CASE("the ground's top is read from the voxels, as generated, dug or built",
          "[globe][world]") {
    MadeWorld made{earth()};
    World& world = made.world();
    const double thickness = earth().layers().thickness;
    const double lod = hog::globe::voxel_lod(earth());
    for (std::uint64_t k = 0; k < 8; ++k) {
        const DVec3 direction =
            hog::globe::center(hog::globe::cell_under(somewhere(k + 50), earth().layers().level));
        made.cover(direction, 1);
        const double generated = hog::globe::ground_below(earth(), direction, lod).value_or(0.0);
        CHECK(std::abs(hog::globe::top_of(world, direction).value_or(0.0) - generated) < thickness);
    }

    const DVec3 direction =
        hog::globe::center(hog::globe::cell_under(somewhere(61), earth().layers().level));
    made.cover(direction);
    const double ground = hog::globe::top_of(world, direction).value_or(0.0);
    hog::globe::dig(world, direction * ground, 3.0);
    CHECK(hog::globe::top_of(world, direction).value_or(0.0) < ground - 2.5);
    // A ball built floating over it is the top; under it, the hole's floor is found.
    hog::globe::build(world, direction * (ground + 6.0), 2.0, Material::Granite);
    CHECK(std::abs(hog::globe::top_of(world, direction).value_or(0.0) - (ground + 8.0)) <
          thickness);
    const double floor = hog::globe::surface_under(world, direction, ground + 3.5).value_or(0.0);
    CHECK(floor < ground - 2.5);
    CHECK(floor > ground - 3.5 - thickness);
}

TEST_CASE("the tile columns near a place are all those with ground in reach, across a face's edge",
          "[globe][world]") {
    const hog::globe::Layers& layers = earth().layers();
    const double radius = earth().planet().body.radius;
    const double tile = hog::globe::Volume::tile_side * layers.thickness;
    constexpr double reach = 1000.0;
    const auto check = [&](DVec3 direction) {
        const DVec3 centre = direction * radius;
        const std::vector<Site> corners = hog::globe::tile_columns_near(layers, centre, reach);
        CHECK(std::ranges::is_sorted(corners));
        CHECK(std::ranges::adjacent_find(corners) == corners.end());
        for (const Site& corner : corners) {
            CHECK(corner.i % hog::globe::Volume::tile_side == 0);
            CHECK(corner.j % hog::globe::Volume::tile_side == 0);
            const hog::globe::Cell middle = hog::globe::column_of(
                {.face = corner.face, .i = corner.i + 32, .j = corner.j + 32}, layers);
            CHECK(glm::length((hog::globe::center(middle) * radius) - centre) <=
                  reach + (2.0 * tile));
        }
        // Ground anywhere in reach is in one of them.
        const DVec3 east = glm::normalize(glm::cross(DVec3{0.0, 0.0, 1.0}, direction));
        const DVec3 north = glm::cross(direction, east);
        std::set<int> faces;
        const double step = tile / 4.0;
        const auto steps = static_cast<int>(reach / step);
        for (int a = -steps; a <= steps; ++a) {
            for (int b = -steps; b <= steps; ++b) {
                const double x = a * step;
                const double y = b * step;
                if (std::hypot(x, y) > reach) {
                    continue;
                }
                const hog::globe::Cell cell = hog::globe::cell_under(
                    glm::normalize(centre + (east * x) + (north * y)), layers.level);
                const Site corner{.face = cell.face, .i = cell.i / 64 * 64, .j = cell.j / 64 * 64};
                CHECK(std::ranges::binary_search(corners, corner));
                faces.insert(cell.face);
            }
        }
        return faces.size();
    };
    CHECK(check(glm::normalize(DVec3{0.3, 0.4, 0.86})) == 1);
    CHECK(check(glm::normalize(DVec3{1.0, 1.0 - 1e-4, 0.2})) == 2);
}

TEST_CASE("a world of voxels alone reads its volume as one made over the generator does",
          "[globe][world]") {
    MadeWorld made{earth()};
    const DVec3 up = glm::normalize(DVec3{0.3, 0.4, 0.86});
    made.cover(up);
    const World alone{earth().planet(), earth().layers(), made.volume()};
    CHECK_FALSE(alone.has_generator());
    CHECK(made.world().has_generator());
    CHECK(alone.layers() == earth().layers());
    CHECK(&alone.planet() == &earth().planet());
    const std::optional<double> ground = hog::globe::top_of(made.world(), up);
    REQUIRE(ground);
    CHECK(hog::globe::top_of(alone, up) == ground);
    const hog::globe::Cell cell = hog::globe::cell_under(up, earth().layers().level);
    const Site site{
        .face = cell.face,
        .i = cell.i,
        .j = cell.j,
        .layer = static_cast<std::uint32_t>(earth().layers().layer_at(ground.value_or(0.0) - 3.0))};
    CHECK(alone.at(site) == made.world().at(site));
}

TEST_CASE("a world of tiles held alone reads them as the world they were made from",
          "[globe][world]") {
    MadeWorld made{earth()};
    const DVec3 up = glm::normalize(DVec3{-0.5, 0.2, 0.84});
    made.cover(up);
    const hog::globe::Cell cell = hog::globe::cell_under(up, earth().layers().level);
    const Site base{.face = cell.face, .i = cell.i, .j = cell.j};
    const std::optional<hog::globe::TileColumn> column = made.volume().column(base);
    REQUIRE(column);
    hog::globe::HeldTiles held;
    constexpr std::uint32_t side = hog::globe::Volume::tile_side;
    for (const auto& [k, tree] : column.value().tiles) {
        held.put({.level = 0, .face = cell.face, .i = cell.i / side, .j = cell.j / side, .k = k},
                 std::make_shared<const hog::globe::VoxelTree>(tree));
    }
    hog::globe::Volume empty;
    World alone{earth().planet(), earth().layers(), empty};
    CHECK_FALSE(hog::globe::top_of(alone, up));
    alone.read_tiles(&held);
    const std::optional<double> ground = hog::globe::top_of(made.world(), up);
    REQUIRE(ground);
    CHECK(hog::globe::top_of(alone, up) == ground);
    for (const double under : {0.5, 3.0, 12.0}) {
        const Site site{
            .face = cell.face,
            .i = cell.i,
            .j = cell.j,
            .layer = static_cast<std::uint32_t>(earth().layers().layer_at(ground.value() - under))};
        CHECK(alone.at(site) == made.world().at(site));
    }
    const hog::globe::TileKey key{.level = 0,
                                  .face = cell.face,
                                  .i = cell.i / side,
                                  .j = cell.j / side,
                                  .k = column.value().tiles.back().first};
    CHECK(alone.tile(key) == held.tile(key));
    CHECK(alone.tile_over(key).value().key == key);

    // Held only coarser, a voxel is the coarse one standing for it.
    const hog::globe::TileKey coarse{.level = 3, .face = cell.face, .i = 0, .j = 0, .k = 0};
    held.put(coarse,
             std::make_shared<const hog::globe::VoxelTree>(
                 side, hog::globe::Cube{.solid = Material::Basalt, .solid_amount = Voxel::full}));
    const hog::globe::TileKey under{.level = 0, .face = cell.face, .i = 2, .j = 5, .k = 1};
    CHECK(alone.tile_over(under).value().key == coarse);
    CHECK(alone.at({.face = cell.face, .i = 2 * side, .j = 5 * side, .layer = side}).solid ==
          Material::Basalt);
}
