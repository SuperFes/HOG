#include "fine_planet.hpp"

#include <hog/globe/brick.hpp>
#include <hog/globe/cell.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/planet.hpp>
#include <hog/globe/voxel.hpp>
#include <hog/globe/voxel_store.hpp>
#include <hog/globe/world.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <span>
#include <vector>

using hog::DVec3;
using hog::globe::BrickSite;
using hog::globe::BrickVoxels;
using hog::globe::Generator;
using hog::globe::Material;
using hog::globe::Site;
using hog::globe::Voxel;
using hog::globe::VoxelStore;
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

TEST_CASE("an unchanged world is what the generator says, voxel by voxel", "[globe][world]") {
    VoxelStore store{earth().layers()};
    const World world{earth(), store};
    const BrickSite brick = ground_at(earth(), somewhere(11)).brick;
    const BrickVoxels voxels = world.brick(brick);
    std::set<Material> solids;
    int inside = 0;
    for (int index = 0; index < hog::globe::brick_voxels; index += 7) {
        const Voxel voxel = voxels[static_cast<std::size_t>(index)];
        CHECK(world.at(hog::globe::site_in(brick, index)) == voxel);
        inside += voxel.distance < 0 ? 1 : 0;
        solids.insert(voxel.solid);
    }
    // The ground passes through it.
    CHECK(inside > 0);
    CHECK(inside < 74);
    CHECK(store.size() == 0);
}

TEST_CASE("writing what's generated holds nothing", "[globe][world]") {
    VoxelStore store{earth().layers()};
    World world{earth(), store};
    const BrickSite brick = ground_at(earth(), somewhere(3)).brick;
    world.write(brick, world.brick(brick));
    CHECK(store.size() == 0);
    BrickVoxels voxels = world.brick(brick);
    voxels[5].condition = 100;
    world.write(brick, voxels);
    CHECK(store.size() == 1);
    voxels[5].condition = Voxel::sound;
    world.write(brick, voxels);
    CHECK(store.size() == 0);
}

TEST_CASE("digging opens a ball in the ground and building fills one", "[globe][world]") {
    VoxelStore store{earth().layers()};
    World world{earth(), store};
    const DVec3 direction = somewhere(29);
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
    CHECK(world.at(site_at(ground.radius)).distance > 0);
    CHECK(world.at(site_at(ground.radius)).solid == Material::Air);
    CHECK(world.at(site_at(ground.radius - 2.5)).distance > 0);
    CHECK(world.at(site_at(ground.radius - 3.5)).distance < 0);
    // Near its wall it faces in, to the middle; far from its wall it faces nowhere.
    const DVec3 floor_normal =
        hog::globe::unpacked_normal(world.at(site_at(ground.radius - 2.75)).normal);
    CHECK(glm::dot(floor_normal, direction) > 0.99);
    CHECK(world.at(site_at(ground.radius)).normal == Voxel::no_normal);
    const std::size_t dug = store.size();
    // Two bricks or more each way, and some more above and below it.
    CHECK(dug >= 4);
    CHECK(dug < 60);

    hog::globe::build(world, centre, 2.0, Material::Granite);
    CHECK(world.at(site_at(ground.radius)).distance < 0);
    CHECK(world.at(site_at(ground.radius)).solid == Material::Granite);
    CHECK(world.at(site_at(ground.radius - 2.6)).distance > 0);
    const Site built_side = site_at(ground.radius - 1.75);
    const DVec3 built_normal = hog::globe::unpacked_normal(world.at(built_side).normal);
    CHECK(glm::dot(built_normal,
                   glm::normalize(hog::globe::middle_of(built_side, layers) - centre)) > 0.9998);

    // A ball dug out of the open sky changes nothing.
    VoxelStore untouched{layers};
    World sky{earth(), untouched};
    hog::globe::dig(sky, direction * (ground.radius + 200.0), 3.0);
    CHECK(untouched.size() == 0);
}

TEST_CASE("a big hole costs little more than its walls", "[globe][world]") {
    VoxelStore store{earth().layers()};
    World world{earth(), store};
    const DVec3 direction = somewhere(41);
    const Ground ground = ground_at(earth(), direction);
    hog::globe::dig(world, direction * (ground.radius - 12.0), 10.0);
    // Under a third of what its voxels held in full would take: inside it, only the rock's
    // warmth with depth still differs between voxels.
    CHECK(store.size() > 100);
    CHECK(store.bytes() < store.size() * sizeof(BrickVoxels) / 3);
}

TEST_CASE("a ball dug across a cube's edge opens on both faces", "[globe][world]") {
    VoxelStore store{earth().layers()};
    World world{earth(), store};
    const DVec3 edge = glm::normalize(DVec3{1.0, 1.0, 0.3});
    const Ground ground = ground_at(earth(), edge);
    hog::globe::dig(world, edge * ground.radius, 4.0);
    std::set<int> faces;
    for (const BrickSite& site : store.sites()) {
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

TEST_CASE("voxels filled far apart are the world's where changed, and coarser ground elsewhere",
          "[globe][world]") {
    VoxelStore store{earth().layers()};
    World world{earth(), store};
    const hog::globe::Layers& layers = earth().layers();
    const Ground ground = ground_at(earth(), somewhere(3));
    const DVec3 direction = glm::normalize(somewhere(3));
    hog::globe::dig(world, direction * ground.radius, 6.0);
    constexpr int apart = 4;
    const hog::globe::Cell middle = hog::globe::cell_under(direction, layers.level);
    std::vector<hog::globe::Cell> cells;
    for (int b = -4; b <= 4; ++b) {
        for (int a = -4; a <= 4; ++a) {
            cells.push_back(hog::globe::next_to(middle, a * apart, b * apart).value_or(middle));
        }
    }
    const double lod = hog::globe::voxel_lod(earth()) * apart;
    const std::vector<Generator::Column> columns = earth().columns(cells, lod, apart);
    const int bottom = layers.layer_at(ground.radius) - 40;
    constexpr int count = 20;
    const std::vector<Voxel> voxels = world.fill(cells, columns, bottom, count, apart);
    int held = 0;
    int past_band = 0;
    for (std::size_t k = 0; k < cells.size(); ++k) {
        for (int step = 0; step < count; ++step) {
            const Site site{.face = cells[k].face,
                            .i = cells[k].i,
                            .j = cells[k].j,
                            .layer = static_cast<std::uint32_t>(bottom + (step * apart))};
            const Voxel voxel =
                voxels[(k * static_cast<std::size_t>(count)) + static_cast<std::size_t>(step)];
            if (store.brick(hog::globe::brick_of(site)) != nullptr) {
                Voxel changed = world.at(site);
                changed.distance =
                    hog::globe::distance_in_mm(hog::globe::metres(changed.distance) / apart);
                CHECK(voxel == changed);
                ++held;
                continue;
            }
            hog::globe::Sample sample =
                columns[k].sample(layers.middle(static_cast<int>(site.layer)));
            sample.distance /= apart;
            CHECK(voxel == voxel_from(sample, distance_band(layers)));
            past_band += std::abs(hog::globe::metres(voxel.distance) * apart) >
                                     distance_band(layers) + 1.0 &&
                                 voxel.normal != Voxel::no_normal
                             ? 1
                             : 0;
        }
    }
    CHECK(held > 0);
    CHECK(past_band > 0);
}

TEST_CASE("columns filled together are the world voxel by voxel, dug or not", "[globe][world]") {
    VoxelStore store{earth().layers()};
    World world{earth(), store};
    const hog::globe::Layers& layers = earth().layers();
    const DVec3 edge = glm::normalize(DVec3{1.0, 1.0, 0.3});
    const Ground ground = ground_at(earth(), edge);
    hog::globe::dig(world, edge * ground.radius, 2.0);
    // A row of cells across the cube's edge, through the hole.
    const hog::globe::Cell middle = hog::globe::cell_under(edge, layers.level);
    std::vector<hog::globe::Cell> cells{middle};
    for (int step = 0; step < 6; ++step) {
        const auto next = hog::globe::next_to(cells.back(), 1, 0);
        REQUIRE(next);
        cells.push_back(next.value_or(middle));
    }
    const std::vector<Generator::Column> columns =
        earth().columns(cells, hog::globe::voxel_lod(earth()));
    const int bottom = layers.layer_at(ground.radius) - 12;
    constexpr int count = 24;
    const std::vector<Voxel> voxels = world.fill(cells, columns, bottom, count);
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
            dug = dug || (store.at(site) && voxel.distance > 0 &&
                          glm::length(middle_of(site, layers)) < ground.radius);
        }
    }
    CHECK(faces.size() == 2);
    CHECK(dug);
}

TEST_CASE("bricks fill from the generator", "[.bench][globe][world]") {
    for (const char* name : {"earth", "mars", "venus", "moon"}) {
        const Generator generator{hog::globe::preset(name).value_or(hog::globe::Planet{})};
        std::vector<BrickSite> bricks;
        bricks.reserve(40);
        for (std::uint64_t k = 0; k < 40; ++k) {
            bricks.push_back(ground_at(generator, somewhere(k)).brick);
        }
        const auto start = std::chrono::steady_clock::now();
        std::size_t inside = 0;
        for (const BrickSite& brick : bricks) {
            for (const Voxel& voxel : hog::globe::generated(generator, brick)) {
                inside += voxel.distance < 0 ? 1U : 0U;
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
        WARN(name << ": " << took.count() * 1e3 / static_cast<double>(bricks.size())
                  << " ms a brick, " << alone.count() * 1e3 / static_cast<double>(bricks.size())
                  << " ms with slopes of their own (" << inside << " inside)");
    }
}

TEST_CASE("a voxel's solid amount keeps to its distance through digging and building",
          "[globe][world][amounts]") {
    VoxelStore store{earth().layers()};
    World world{earth(), store};
    const DVec3 direction = somewhere(11);
    const Ground ground = ground_at(earth(), direction);
    const double thickness = earth().layers().thickness;
    hog::globe::dig(world, direction * ground.radius, 3.0);
    hog::globe::build(world, direction * (ground.radius - 1.0), 1.5, Material::Granite);
    std::size_t checked = 0;
    for (const BrickSite site : store.changed_since(0)) {
        const hog::globe::Brick* brick = store.brick(site);
        REQUIRE(brick != nullptr);
        BrickVoxels voxels{};
        brick->unpack(voxels);
        for (const Voxel& voxel : voxels) {
            const std::uint16_t expected =
                hog::globe::amount_of(0.5 - (hog::globe::metres(voxel.distance) / thickness));
            // Within what millimetres of distance can tell of an amount.
            const int apart = std::abs(int{voxel.solid_amount} - int{expected});
            CHECK(apart <= 40);
            ++checked;
        }
    }
    CHECK(checked > 1'000);
}
