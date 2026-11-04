#include "bed.hpp"

#include <hog/globe/cell.hpp>
#include <hog/globe/voxel.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <utility>

using namespace hog::globe::bed;
using hog::DVec3;
using hog::globe::Material;

namespace {

// A bowl dug into gentle ground, `across` metres wide and `deep` deep, holding water `fall` metres
// under the ground at its rim.
struct Lake {
    DVec3 up{0.0};
    DVec3 rim{0.0}; // the ground under the bowl's middle, before it was dug
    double level = 0.0;
};

Lake make_lake(World& world, double across, double deep, double fall) {
    const DVec3 up = ground_between(earth(), 50.0, 400.0);
    const double ground = hog::globe::ground_below(earth(), up, 0.5).value_or(0.0);
    // A ball whose lowest part is the bowl.
    const double radius = ((across * across / 4.0) + (deep * deep)) / (2.0 * deep);
    const DVec3 centre = up * (ground + radius - deep);
    hog::globe::dig(world, centre, radius);
    const Lake lake{.up = up, .rim = up * ground, .level = ground - fall};
    const double thickness = earth().layers().thickness;
    fill_hollow(world, centre, radius, [&](hog::globe::Voxel& voxel, double bottom) {
        if (bottom < lake.level) {
            voxel.liquid = Material::Water;
            voxel.liquid_amount =
                hog::globe::amount_of(std::min(1.0, (lake.level - bottom) / thickness));
        }
    });
    return lake;
}

} // namespace

TEST_CASE("a lake filling its bowl meshes level, with a closed shore", "[globe][lake]") {
    VoxelStore store{earth().layers()};
    World world{earth(), store};
    const Lake lake = make_lake(world, 40.0, 8.0, 1.5);
    const double thickness = earth().layers().thickness;
    const SurfaceMesh mesh =
        mesh_of(world, block_round(hog::globe::chunk_under(lake.up, earth().layers())));
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
    CHECK(surface > 1'000);
    CHECK(off_level * 50 < surface);
    // The sheet of water runs on under the shore, so nothing near the lake is left open.
    for (const DVec3 side : open_sides(mesh)) {
        const DVec3 out = side - lake.rim;
        CHECK(glm::length(out - (glm::dot(out, lake.up) * lake.up)) >= 16.0);
    }
}

TEST_CASE("a lake's water is closed by the ground, never stopping short of a lower shore",
          "[globe][lake]") {
    auto planet = hog::globe::preset("earth").value_or(hog::globe::Planet{});
    planet.radius = 4'000.0;
    planet.floor_depth = 100.0;
    planet.sky_height = 500.0;
    planet.wear = 0.03;
    const Generator generator{planet};
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
                        if (!beside || generator.pond_at(hog::globe::center(*beside))) {
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
