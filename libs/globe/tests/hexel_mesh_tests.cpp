#include <hog/globe/hexel_mesh.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <numbers>

using hog::DVec3;
using hog::globe::Globe;
using hog::globe::HexCell;
using hog::globe::Material;

namespace {

void fill(Globe& globe, int below, Material material) {
    for (std::uint64_t index = 0; index < hog::globe::cell_count(globe.level()); ++index) {
        const auto column = globe.column(hog::globe::hex_cell_at_index(globe.level(), index));
        for (int shell = 0; shell < below; ++shell) {
            column[static_cast<std::size_t>(shell)].material = material;
        }
    }
}

// Every triangle turns anticlockwise about its normal, and the normal points away from the centre
// of the hexel it came from as `outward` says.
void check_faces(const hog::globe::HexelMesh& mesh) {
    for (std::size_t at = 0; at < mesh.indices.size(); at += 3) {
        const auto& a = mesh.vertices[mesh.indices[at]];
        const auto& b = mesh.vertices[mesh.indices[at + 1]];
        const auto& c = mesh.vertices[mesh.indices[at + 2]];
        const auto turn = glm::cross(b.position - a.position, c.position - a.position);
        CHECK(glm::dot(turn, a.normal) > 0.0F);
    }
}

} // namespace

TEST_CASE("ground under air shows only its top, covering the sphere", "[globe][hexel_mesh]") {
    constexpr double radius = 10'000.0;
    Globe globe{3, {.inner = 0.0, .outer = 1.5 * radius, .count = 8}};
    // Up to the radius, about where the ground stands.
    fill(globe, 3, Material::Granite);
    const double top = globe.shells().ceiling(2);
    const auto mesh = hog::globe::mesh_hexels(globe, DVec3{0.0});
    check_faces(mesh);
    double area = 0.0;
    for (std::size_t at = 0; at < mesh.indices.size(); at += 3) {
        const auto& a = mesh.vertices[mesh.indices[at]];
        const auto& b = mesh.vertices[mesh.indices[at + 1]];
        const auto& c = mesh.vertices[mesh.indices[at + 2]];
        CHECK(a.hexel.material == Material::Granite);
        // Facing straight out from the centre.
        CHECK(glm::dot(a.normal, glm::normalize(a.position)) > 0.99F);
        area += 0.5 * glm::length(glm::cross(DVec3{b.position - a.position},
                                             DVec3{c.position - a.position}));
    }
    // Flat polygons cut a little inside the round surface.
    const double sphere = 4.0 * std::numbers::pi * top * top;
    CHECK(area < sphere);
    CHECK(area > 0.99 * sphere);
}

TEST_CASE("water shows over the ground it covers, and the ground under the water",
          "[globe][hexel_mesh]") {
    Globe globe{2, {.inner = 0.0, .outer = 6'000.0, .count = 6}};
    fill(globe, 4, Material::Water);
    fill(globe, 3, Material::Basalt);
    const auto mesh = hog::globe::mesh_hexels(globe, DVec3{0.0});
    check_faces(mesh);
    std::size_t water = 0;
    std::size_t basalt = 0;
    for (const auto& vertex : mesh.vertices) {
        (vertex.hexel.material == Material::Water ? water : basalt) += 1;
    }
    CHECK(water == basalt);
    CHECK(water > 0);
}

TEST_CASE("a lone hexel shows all its sides, its top and its floor", "[globe][hexel_mesh]") {
    Globe globe{2, {.inner = 1'000.0, .outer = 1'400.0, .count = 4}};
    for (const std::uint64_t index : {std::uint64_t{0}, std::uint64_t{77}}) {
        Globe lone = globe;
        const HexCell cell = hog::globe::hex_cell_at_index(2, index);
        lone.at(cell, 2).material = Material::Limestone;
        const auto mesh = hog::globe::mesh_hexels(lone, hog::globe::center(cell) * 1'200.0);
        check_faces(mesh);
        const auto sides = static_cast<std::size_t>(hog::globe::neighbors(cell).count);
        // Two polygons of `sides` corners and `sides` quads.
        CHECK(mesh.vertices.size() == (2 * sides) + (4 * sides));
        CHECK(mesh.indices.size() == (std::size_t{6} * (sides - 2)) + (std::size_t{6} * sides));
        // Every face turns away from the hexel's middle.
        const DVec3 middle = hog::globe::center(cell) * 1'250.0;
        for (std::size_t at = 0; at < mesh.indices.size(); at += 3) {
            const auto& vertex = mesh.vertices[mesh.indices[at]];
            const DVec3 position = DVec3{vertex.position} + mesh.origin;
            CHECK(glm::dot(DVec3{vertex.normal}, position - middle) > 0.0);
        }
    }
}

TEST_CASE("a window shows its cut where its columns end", "[globe][hexel_mesh]") {
    const HexCell middle = hog::globe::hex_cell_at_index(5, 4'321);
    Globe near{
        5, {.inner = 9'000.0, .outer = 9'100.0, .count = 1}, hog::globe::disc(middle, 1), 40};
    for (std::uint64_t index = 0; index < near.size(); ++index) {
        near.column_at(index)[0].material = Material::Granite;
    }
    const auto mesh = hog::globe::mesh_hexels(near, DVec3{0.0});
    check_faces(mesh);
    std::size_t sides = 0;
    for (std::size_t at = 0; at < mesh.indices.size(); at += 3) {
        const auto& a = mesh.vertices[mesh.indices[at]];
        sides += std::abs(glm::dot(a.normal, glm::normalize(a.position))) < 0.1F ? 1 : 0;
    }
    // The six round the middle each show three sides out, two triangles a side.
    CHECK(sides == 6 * 3 * 2);
}
