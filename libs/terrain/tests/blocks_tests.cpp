#include <hog/core/math.hpp>
#include <hog/planet/projection.hpp>
#include <hog/planet/volume.hpp>
#include <hog/terrain/blocks.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <optional>
#include <random>

using hog::planet::CellId;
using hog::planet::Contents;
using hog::planet::Material;
using hog::planet::Volume;
using hog::planet::VolumeCell;
using hog::terrain::block_mesh;
using hog::terrain::BlockMesh;

namespace {

constexpr double radius = 1'000'000.0;
constexpr int region_level = 3;

const Contents air{.material = Material::Air, .temperature = 250.0F, .surface = std::nullopt};
const Contents rock{.material = Material::Granite, .temperature = 300.0F, .surface = std::nullopt};
const Contents water{.material = Material::Water, .temperature = 280.0F, .surface = std::nullopt};

// A cell some ten metres across and sixteen tall at the planet's radius, under the pole.
[[nodiscard]] VolumeCell block() {
    return VolumeCell::from(hog::planet::cell_at(hog::DVec3{0.0, 0.0, 1.0}, 16), 14, 8192);
}

[[nodiscard]] CellId region_of(VolumeCell cell) {
    return cell.column().parent(region_level);
}

[[nodiscard]] std::size_t quads(const BlockMesh& mesh) {
    return mesh.indices.size() / 6;
}

[[nodiscard]] double area(const BlockMesh& mesh) {
    double total = 0.0;
    for (std::size_t index = 0; index + 2 < mesh.indices.size(); index += 3) {
        const hog::DVec3 a{mesh.vertices[mesh.indices[index]].position};
        const hog::DVec3 b{mesh.vertices[mesh.indices[index + 1]].position};
        const hog::DVec3 c{mesh.vertices[mesh.indices[index + 2]].position};
        total += glm::length(glm::cross(b - a, c - a)) / 2.0;
    }
    return total;
}

[[nodiscard]] std::size_t coloured(const BlockMesh& mesh, Material material) {
    std::size_t count = 0;
    for (std::size_t index = 0; index < mesh.vertices.size(); index += 4) {
        count += mesh.vertices[index].colour == hog::terrain::block_colour(material) ? 1 : 0;
    }
    return count;
}

} // namespace

TEST_CASE("a lone block shows all six sides, facing out", "[terrain][blocks]") {
    Volume volume{air, region_level};
    volume.set(block(), rock);
    const BlockMesh mesh = block_mesh(volume, radius, region_of(block()));
    REQUIRE(quads(mesh) == 6);
    CHECK(mesh.vertices.size() == 24);
    const hog::DVec3 middle = hog::planet::volume_center(block(), radius) - mesh.origin;
    for (std::size_t index = 0; index + 2 < mesh.indices.size(); index += 3) {
        const hog::DVec3 a{mesh.vertices[mesh.indices[index]].position};
        const hog::DVec3 b{mesh.vertices[mesh.indices[index + 1]].position};
        const hog::DVec3 c{mesh.vertices[mesh.indices[index + 2]].position};
        const hog::DVec3 wound = glm::cross(b - a, c - a);
        // Counter-clockwise seen from outside, and lit from outside.
        REQUIRE(glm::dot(wound, a - middle) > 0.0);
        REQUIRE(glm::dot(hog::DVec3{mesh.vertices[mesh.indices[index]].normal}, a - middle) > 0.0);
    }
}

TEST_CASE("sides between blocks don't show", "[terrain][blocks]") {
    Volume volume{air, region_level};
    volume.set(block(), rock);
    volume.set(VolumeCell::from(block().column(), 14, 8193), rock);
    CHECK(quads(block_mesh(volume, radius, region_of(block()))) == 10);
}

TEST_CASE("ground shows only its top", "[terrain][blocks]") {
    Volume volume{air, region_level};
    for (int face = 0; face < hog::planet::face_count; ++face) {
        volume.set(VolumeCell::from(CellId::from_face(face), 1, 0), rock);
    }
    const CellId region = region_of(block());
    const BlockMesh mesh = block_mesh(volume, radius, region);
    REQUIRE(quads(mesh) == 1);
    const hog::DVec3 up = hog::planet::cell_center(region);
    for (const auto& vertex : mesh.vertices) {
        const hog::WorldPosition at = mesh.origin + hog::DVec3{vertex.position};
        CHECK(std::abs(glm::length(at.metres()) - radius) < 0.1);
        CHECK(glm::dot(hog::DVec3{vertex.normal}, up) > 0.99);
    }
}

TEST_CASE("a side against finer cells shows once, all of it", "[terrain][blocks]") {
    Volume lone{air, region_level};
    lone.set(block(), rock);
    const double whole = area(block_mesh(lone, radius, region_of(block())));

    Volume volume{air, region_level};
    volume.set(block(), rock);
    // Air beside it and over it, split finer than it.
    Contents warmer = air;
    warmer.temperature = 260.0F;
    const auto around = hog::planet::face_neighbors(block());
    volume.set(around[static_cast<std::size_t>(hog::planet::Side::PlusU)].across_child(0), warmer);
    volume.set(around[static_cast<std::size_t>(hog::planet::Side::Up)].shell_child(0), warmer);
    const BlockMesh mesh = block_mesh(volume, radius, region_of(block()));
    CHECK(quads(mesh) > 6);
    CHECK(std::abs(area(mesh) - whole) < 1.0e-3 * whole);
}

TEST_CASE("rock shows to the water over it and water to the air", "[terrain][blocks]") {
    Volume volume{air, region_level};
    volume.set(block(), rock);
    volume.set(VolumeCell::from(block().column(), 14, 8193), water);
    const BlockMesh mesh = block_mesh(volume, radius, region_of(block()));
    CHECK(coloured(mesh, Material::Granite) == 6);
    CHECK(coloured(mesh, Material::Water) == 5);
}

TEST_CASE("a cell finer one way shows its side to one finer the other way", "[terrain][blocks]") {
    Volume lone{air, region_level};
    lone.set(block(), rock);
    const double whole = area(block_mesh(lone, radius, region_of(block())));

    // Thin layers of rock against tall, narrow columns of air, so neither holds the other's side.
    Volume volume{air, region_level};
    Contents warmer = air;
    warmer.temperature = 260.0F;
    volume.set(block().shell_child(0), rock);
    volume.set(block().shell_child(1), rock);
    const auto around = hog::planet::face_neighbors(block());
    const VolumeCell beside = around[static_cast<std::size_t>(hog::planet::Side::MinusV)];
    volume.set(beside.across_child(1).shell_parent(), warmer);
    const BlockMesh mesh = block_mesh(volume, radius, region_of(block()));
    CHECK(std::abs(area(mesh) - whole) < 1.0e-3 * whole);
}

TEST_CASE("every side between rock and air shows once", "[terrain][blocks]") {
    std::mt19937_64 random{3};
    Volume volume{air, region_level};
    std::uniform_int_distribution<std::uint32_t> nearby(0, 3);
    std::uniform_int_distribution<int> levels(0, 2);
    const auto [face, i, j, level] = block().column().to_face_ij();
    Contents warmer = air;
    warmer.temperature = 260.0F;
    for (int n = 0; n < 200; ++n) {
        const CellId column =
            CellId::from_face_ij(face, i + nearby(random), j + nearby(random), level);
        VolumeCell cell = VolumeCell::from(column, 14, 8190 + nearby(random));
        for (int split = levels(random); split > 0; --split) {
            cell = n % 2 == 0 ? cell.across_child(static_cast<int>(nearby(random)))
                              : cell.shell_child(static_cast<int>(nearby(random) % 2));
        }
        volume.set(cell, n % 3 == 0 ? warmer : rock);
    }
    // Every side drawn faces air, from rock: a step out along its normal is air and a step in is
    // rock.
    const BlockMesh mesh = block_mesh(volume, radius, region_of(block()));
    REQUIRE(!mesh.indices.empty());
    for (std::size_t index = 0; index + 3 < mesh.vertices.size(); index += 4) {
        hog::DVec3 middle{0.0};
        for (std::size_t corner = 0; corner < 4; ++corner) {
            middle += hog::DVec3{mesh.vertices[index + corner].position} / 4.0;
        }
        const hog::DVec3 normal{mesh.vertices[index].normal};
        const auto material = [&](double along) {
            const auto leaf =
                hog::planet::leaf_at(volume, radius, mesh.origin + (middle + (normal * along)));
            return leaf.value_or(Volume::Leaf{}).contents.material;
        };
        REQUIRE(material(0.01) == Material::Air);
        REQUIRE(material(-0.01) == Material::Granite);
    }
}
