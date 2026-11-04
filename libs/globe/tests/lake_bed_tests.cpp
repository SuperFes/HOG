#include "bed.hpp"

#include <hog/globe/cell.hpp>
#include <hog/globe/voxel.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <numbers>
#include <utility>

using namespace hog::globe::bed;
using hog::DVec3;
using hog::globe::Material;

namespace {

// Voxel sizes a world may be run at, in metres: what a rule has to hold at.
constexpr std::array sizes{0.5, 1.0, 2.5, 5.0};

// The planet at a voxel size, kept so each is made once.
const Generator& working(double voxel) {
    static std::map<double, std::unique_ptr<Generator>> made;
    auto& held = made[voxel];
    if (!held) {
        auto planet = hog::globe::Planet{};
        planet.voxel = voxel;
        held = std::make_unique<Generator>(planet);
    }
    return *held;
}

// A small worn planet with lakes in it, at a voxel size, kept so each is made once.
const Generator& lake_world(double voxel) {
    static std::map<double, std::unique_ptr<Generator>> made;
    auto& held = made[voxel];
    if (!held) {
        auto planet = hog::globe::preset("earth").value_or(hog::globe::Planet{});
        planet.radius = 4'000.0;
        planet.floor_depth = 100.0;
        planet.sky_height = 500.0;
        planet.wear = 0.03;
        planet.voxel = voxel;
        held = std::make_unique<Generator>(planet);
    }
    return *held;
}

// Whether a lake has flooded the column, its rim aside.
bool flooded(const Generator& generator, hog::globe::Cell column) {
    const auto& shape = generator.shape();
    const auto found =
        shape.ponds.find(hog::globe::index_of({.level = static_cast<std::uint8_t>(shape.cut_level),
                                               .face = column.face,
                                               .i = column.i >> 6,
                                               .j = column.j >> 6}));
    return found != shape.ponds.end() &&
           std::ranges::any_of(found->second.layers, [&](const auto& layer) {
               return ((layer.rows[column.i & 63U] >> (column.j & 63U)) & 1U) != 0U;
           });
}

// A bowl dug into gentle ground, about `across` metres wide, holding water `fall` metres under
// the lowest ground round its rim, so the ground itself holds the water in.
struct Lake {
    DVec3 up{0.0};
    DVec3 rim{0.0}; // the ground under the bowl's middle, before it was dug
    double level = 0.0;
    double across = 0.0;
};

Lake make_lake(World& world, double across, double fall) {
    const Generator& generator = world.generator();
    const DVec3 up = ground_between(generator, 50.0, 400.0);
    const double ground = hog::globe::ground_below(generator, up, 0.5).value_or(0.0);
    const DVec3 east = glm::normalize(glm::cross(DVec3{0.0, 0.0, 1.0}, up));
    const DVec3 north = glm::cross(up, east);
    double lowest = ground;
    for (int k = 0; k < 16; ++k) {
        const double turn = static_cast<double>(k) * std::numbers::pi / 8.0;
        const DVec3 round =
            glm::normalize(up + (((std::cos(turn) * east) + (std::sin(turn) * north)) * 0.75 *
                                 across / generator.planet().radius));
        lowest = std::min(lowest, hog::globe::ground_below(generator, round, 0.5).value_or(ground));
    }
    const double level = lowest - fall;
    const double deep = (ground - level) + (3.0 * generator.layers().thickness);
    // A ball whose lowest part is the bowl.
    const double radius = ((across * across / 4.0) + (deep * deep)) / (2.0 * deep);
    const DVec3 centre = up * (ground + radius - deep);
    hog::globe::dig(world, centre, radius);
    const double thickness = generator.layers().thickness;
    fill_hollow(world, centre, radius, [&](hog::globe::Voxel& voxel, double bottom) {
        if (bottom < level) {
            voxel.liquid = Material::Water;
            voxel.liquid_amount =
                hog::globe::amount_of(std::min(1.0, (level - bottom) / thickness));
        }
    });
    return {.up = up, .rim = up * ground, .level = level, .across = across};
}

} // namespace

TEST_CASE("a lake filling its bowl meshes level, with a closed shore", "[globe][lake]") {
    const double voxel = GENERATE(from_range(sizes));
    CAPTURE(voxel);
    const Generator& generator = working(voxel);
    VoxelStore store{generator.layers()};
    World world{generator, store};
    const Lake lake = make_lake(world, 32.0 * voxel, 1.5 * voxel);
    const double thickness = generator.layers().thickness;
    const SurfaceMesh mesh =
        mesh_of(world, block_round(hog::globe::chunk_under(lake.up, generator.layers())));
    std::size_t surface = 0;
    std::size_t off_level = 0;
    for (std::uint32_t v = 0; v < mesh.vertices.size(); ++v) {
        const DVec3 point = world_point(mesh, v);
        const bool up = glm::dot(DVec3{mesh.vertices[v].normal}, glm::normalize(point)) > 0.99;
        if (liquid(mesh.vertices[v].material) && up) {
            ++surface;
            off_level += std::abs(glm::length(point) - lake.level) > thickness ? 1U : 0U;
        }
    }
    CHECK(surface > 20);
    CHECK(off_level * 20 < surface);
    // The sheet of water ends under the ground at the shore, not short of it: every edge it ends
    // in lies under the ground the mesh draws, or on it.
    std::size_t open = 0;
    double worst = 0.0;
    for (const auto& [p, q] : open_ends(mesh)) {
        const DVec3 mid = 0.5 * (p + q);
        const DVec3 out = mid - lake.rim;
        if (glm::length(out - (glm::dot(out, lake.up) * lake.up)) >= lake.across) {
            continue;
        }
        const double over = clearance(mesh, mid);
        worst = std::max(worst, over);
        open += over > 0.02 * thickness ? 1U : 0U;
    }
    CAPTURE(worst);
    // The measure itself: over the old ground is open air, and far under it is rock.
    CHECK(clearance(mesh, lake.rim + (lake.up * 4.0 * thickness)) > 0.0);
    CHECK(clearance(mesh, lake.rim - (lake.up * 40.0 * thickness)) < 0.0);
    CHECK(open == 0);
}

TEST_CASE("a lake's water is closed by the ground, never stopping short of a lower shore",
          "[globe][lake]") {
    const double voxel = GENERATE(from_range(sizes));
    CAPTURE(voxel);
    const Generator& generator = lake_world(voxel);
    const auto& planet = generator.planet();
    const auto& shape = generator.shape();
    const int fine_level = shape.cut_level + 6;
    const double lod =
        planet.radius * std::numbers::pi / 2.0 / static_cast<double>(1 << fine_level);
    std::size_t edges = 0;
    std::size_t open = 0;
    for (const auto& [index, pond] : shape.ponds) {
        const hog::globe::Cell home = hog::globe::cell_at_index(shape.cut_level, index);
        for (const auto& layer : pond.layers) {
            for (std::uint32_t a = 0; a < 64; ++a) {
                for (std::uint32_t b = 0; b < 64; ++b) {
                    if (((layer.rows[a] >> b) & 1U) == 0U) {
                        continue;
                    }
                    const hog::globe::Cell column{.level = static_cast<std::uint8_t>(fine_level),
                                                  .face = home.face,
                                                  .i = (home.i << 6) + a,
                                                  .j = (home.j << 6) + b};
                    for (const auto [di, dj] :
                         {std::pair{1, 0}, std::pair{-1, 0}, std::pair{0, 1}, std::pair{0, -1}}) {
                        const auto beside = hog::globe::next_to(column, di, dj);
                        if (!beside || flooded(generator, *beside)) {
                            continue;
                        }
                        ++edges;
                        // Ground beside the water that's under its surface is a wall of water.
                        const double ground =
                            generator.column(hog::globe::center(*beside), lod).ground() -
                            planet.radius;
                        open += static_cast<double>(layer.level) - ground > 0.5 ? 1U : 0U;
                    }
                }
            }
        }
    }
    CHECK(edges > 1'000);
    CHECK(open == 0);
}

TEST_CASE("a lake's water reaches every shore column under its level, at the voxels' own size",
          "[globe][lake]") {
    const double voxel = GENERATE(from_range(sizes));
    CAPTURE(voxel);
    const Generator& generator = lake_world(voxel);
    const auto& shape = generator.shape();
    const int fine_level = shape.cut_level + 6;
    constexpr int reach = 8;
    constexpr int side = (2 * reach) + 1;
    std::size_t spots = 0;
    std::size_t shore = 0;
    std::size_t dry = 0;
    for (const auto& [index, pond] : shape.ponds) {
        const hog::globe::Cell home = hog::globe::cell_at_index(shape.cut_level, index);
        for (const auto& layer : pond.layers) {
            for (std::uint32_t a = 0; a < 64 && spots < 60; a += 7) {
                for (std::uint32_t b = 0; b < 64 && spots < 60; ++b) {
                    const hog::globe::Cell column{.level = static_cast<std::uint8_t>(fine_level),
                                                  .face = home.face,
                                                  .i = (home.i << 6) + a,
                                                  .j = (home.j << 6) + b};
                    const auto beside = hog::globe::next_to(column, 1, 0);
                    if (((layer.rows[a] >> b) & 1U) == 0U || !beside ||
                        flooded(generator, *beside)) {
                        continue;
                    }
                    const auto voxel_column = hog::globe::cell_under(
                        glm::normalize(hog::globe::center(column)), generator.layers().level);
                    const auto corner = hog::globe::next_to(voxel_column, -reach, -reach);
                    // The square of columns must lie on the one face.
                    if (!corner || corner->face != voxel_column.face ||
                        corner->i + side > (1U << corner->level) ||
                        corner->j + side > (1U << corner->level)) {
                        continue;
                    }
                    ++spots;
                    const auto columns =
                        generator.columns(*corner, side, hog::globe::voxel_lod(generator));
                    const auto at = [&](int i, int j) -> const Generator::Column& {
                        return columns[static_cast<std::size_t>((i * side) + j)];
                    };
                    for (int i = 1; i + 1 < side; ++i) {
                        for (int j = 1; j + 1 < side; ++j) {
                            const bool wet = at(i, j).water() > at(i, j).ground();
                            for (const auto [di, dj] : {std::pair{1, 0}, std::pair{-1, 0},
                                                        std::pair{0, 1}, std::pair{0, -1}}) {
                                const auto& near = at(i + di, j + dj);
                                if (wet || near.water() <= near.ground()) {
                                    continue;
                                }
                                // Ground under the water beside it, left dry, is a wall of water.
                                ++shore;
                                dry += at(i, j).ground() < near.water() - 0.25 ? 1U : 0U;
                            }
                        }
                    }
                }
            }
        }
    }
    CHECK(spots > 5);
    CHECK(shore > 20);
    CHECK(dry == 0);
}
