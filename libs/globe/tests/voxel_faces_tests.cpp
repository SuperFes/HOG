#include <hog/globe/cell.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/planet.hpp>
#include <hog/globe/surface_mesh.hpp>
#include <hog/globe/volume_build.hpp>
#include <hog/globe/voxel.hpp>
#include <hog/globe/world.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

using namespace hog::globe;

namespace {

bool solidish(const Voxel& voxel) {
    const auto half = static_cast<std::uint16_t>(Voxel::full / 2);
    return (voxel.solid != Material::Air && voxel.solid_amount >= half) ||
           (voxel.grain != Material::Air && voxel.grain_amount >= half);
}

bool liquidish(const Voxel& voxel) {
    return !solidish(voxel) && voxel.liquid != Material::Air &&
           voxel.liquid_amount >= static_cast<std::uint16_t>(Voxel::full / 2);
}

} // namespace

TEST_CASE("a chunk drawn as voxels has a box face for each solid voxel's open top",
          "[globe][voxel_faces]") {
    const Generator generator{Planet{}};
    const Layers& layers = generator.layers();
    MadeWorld made{generator};
    const World& world = made.world();
    // Land: the highest ground over a coarse sweep.
    hog::DVec3 land{0.0, 0.0, 1.0};
    double top = 0.0;
    for (int k = 0; k < 2000; ++k) {
        const double z = 1.0 - ((2.0 * k + 1.0) / 2000.0);
        const double r = std::sqrt(1.0 - (z * z));
        const hog::DVec3 d{r * std::cos(2.399963 * k), r * std::sin(2.399963 * k), z};
        if (const double ground = generator.column(d, 200.0).ground(); ground > top) {
            top = ground;
            land = d;
        }
    }
    const Cell chunk = chunk_under(land, layers, 0);
    made.cover(land);
    SurfaceMesh mesh;
    mesh.origin = center(chunk) * generator.planet().radius;
    add_chunk_voxels(mesh, world, chunk, Drawn{}, std::nullopt);

    REQUIRE(!mesh.vertices.empty());
    CHECK(mesh.vertices.size() % 4 == 0);
    CHECK(mesh.indices.size() == mesh.vertices.size() / 4 * 6);

    // Faces facing straight up or down, against the world's own voxels column by column.
    const std::uint32_t across = std::uint32_t{1} << (layers.level - chunk.level);
    std::size_t ups = 0;
    for (std::uint32_t b = 0; b < across; ++b) {
        for (std::uint32_t a = 0; a < across; ++a) {
            const std::uint32_t i = (chunk.i * across) + a;
            const std::uint32_t j = (chunk.j * across) + b;
            const int ground = layers.layer_at(
                generator.column(center({.level = static_cast<std::uint8_t>(layers.level),
                                         .face = chunk.face,
                                         .i = i,
                                         .j = j}),
                                 1.0)
                    .ground());
            for (int layer = std::max(ground - 6, 0); layer < ground + 6; ++layer) {
                const Voxel here = world.at({chunk.face, i, j, static_cast<std::uint32_t>(layer)});
                const Voxel over = world.at({chunk.face, i, j, static_cast<std::uint32_t>(layer + 1)});
                const bool open = !solidish(over) && !liquidish(over);
                if ((solidish(here) && (open || liquidish(over))) || (liquidish(here) && open)) {
                    ++ups;
                }
            }
        }
    }
    std::size_t drawn = 0;
    for (std::size_t v = 0; v < mesh.vertices.size(); v += 4) {
        const auto& vertex = mesh.vertices[v];
        const hog::DVec3 up = glm::normalize(mesh.origin + hog::DVec3{vertex.position});
        const double lean = glm::dot(hog::DVec3{vertex.normal}, up);
        CHECK(vertex.material != Material::Air);
        if (lean > 0.999) {
            ++drawn;
        }
    }
    CHECK(ups > 0);
    CHECK(drawn >= ups);
}

TEST_CASE("a world drawn as boxes has its chunks drawn as voxels, and not otherwise",
          "[globe][voxel_faces]") {
    const Generator generator{Planet{}};
    const Layers& layers = generator.layers();
    MadeWorld made{generator};
    World& world = made.world();
    const hog::DVec3 up = glm::normalize(hog::DVec3{0.3, 0.4, 0.86});
    made.cover(up);
    const Cell chunk = chunk_under(up, layers, 0);
    const auto drawn = [&] {
        SurfaceMesh mesh;
        mesh.origin = center(chunk) * generator.planet().radius;
        add_chunk(mesh, world, chunk, Drawn{}, std::nullopt);
        return mesh;
    };
    const SurfaceMesh shaped = drawn();
    world.draw_as_boxes(true);
    const SurfaceMesh boxes = drawn();
    SurfaceMesh direct;
    direct.origin = center(chunk) * generator.planet().radius;
    add_chunk_voxels(direct, world, chunk, Drawn{}, std::nullopt);
    CHECK(boxes.vertices.size() == direct.vertices.size());
    CHECK(boxes.indices == direct.indices);
    CHECK(boxes.vertices.size() % 4 == 0);
    CHECK(shaped.vertices.size() != boxes.vertices.size());
}
