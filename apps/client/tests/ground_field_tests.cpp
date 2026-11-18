#include "sloped_ground.hpp"
#include "welding.hpp"

#include <hog/client/ground_field.hpp>
#include <hog/client/held_pages.hpp>
#include <hog/client/transvoxel.hpp>
#include <hog/globe/material.hpp>
#include <hog/grid/grid.hpp>
#include <hog/grid/kinds.hpp>
#include <hog/grid/page.hpp>
#include <hog/grid/tree.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <utility>
#include <vector>

using hog::DVec3;
using hog::I64Vec3;
using hog::client::GroundField;
using hog::client::ShapeMesh;
using hog::grid::Key;

namespace {

using namespace hog::sloped;

} // namespace

TEST_CASE("the ground's value at a point is how far above the ground it lies",
          "[client][ground_field]") {
    const Sloped sloped;
    GroundField field{sloped.pages};
    for (std::int64_t z = 8; z < 56; z += 3) {
        const I64Vec3 point = page_low + I64Vec3{29, 35, z};
        CAPTURE(z);
        CHECK(std::abs(field.value(point) - above_ground(metres(sloped.grid, point))) < 0.02);
    }
    // Beyond the page, from the top page's coarser nodes.
    const I64Vec3 far{3, 3, 3};
    CHECK(std::abs(field.value(far) - above_ground(metres(sloped.grid, far))) < 1.0);
}

TEST_CASE("an edge is crossed where its finer edges are", "[client][ground_field]") {
    const Sloped sloped;
    GroundField field{sloped.pages};
    for (const I64Vec3 column : {I64Vec3{32, 32, 0}, I64Vec3{8, 56, 0}, I64Vec3{40, 16, 0}}) {
        CAPTURE(column.x, column.y);
        const I64Vec3 low = page_low + column;
        const hog::client::Crossed whole = field.crossing(low, low + I64Vec3{0, 0, 64});
        CHECK(std::abs(above_ground(whole.at)) < 0.02);
        CHECK(glm::dot(DVec3{whole.normal}, normal()) > 0.99);
        CHECK(whole.materials[0] == static_cast<std::uint16_t>(hog::globe::Material::Granite));
        for (const std::int64_t length : {16, 8, 2, 1}) {
            CAPTURE(length);
            for (std::int64_t from = 0; from < 64; from += length) {
                const I64Vec3 a = low + I64Vec3{0, 0, from};
                const I64Vec3 b = a + I64Vec3{0, 0, length};
                if ((field.value(a) < 0.0) != (field.value(b) < 0.0)) {
                    CHECK(glm::length(field.crossing(a, b).at - whole.at) < 1e-9);
                }
            }
        }
    }
}

TEST_CASE("blocks over held ground lie on it and meet finer ones without gaps",
          "[client][ground_field]") {
    const Sloped sloped;
    GroundField field{sloped.pages};
    const std::array<Key, 1> split{page_root.child(5)};
    std::vector<ShapeMesh> meshes;
    for (const hog::client::Block& block : hog::welding::blocks_of(page_root, split, 3)) {
        meshes.push_back(mesh_block(sloped.grid, block, field.field()));
        for (const auto& vertex : meshes.back().vertices) {
            const DVec3 at = meshes.back().origin + DVec3{vertex.position};
            CHECK(std::abs(above_ground(at)) < 0.05);
            CHECK(glm::dot(DVec3{vertex.normal}, normal()) > 0.99);
        }
    }
    const ShapeMesh one = hog::welding::welded(meshes);
    REQUIRE(!one.indices.empty());
    // Every edge is between two triangles but round the page's sides, crossed once each way.
    std::map<std::pair<std::uint32_t, std::uint32_t>, int> directed;
    for (std::size_t at = 0; at < one.indices.size(); at += 3) {
        for (std::size_t side = 0; side < 3; ++side) {
            ++directed[{one.indices[at + side], one.indices[at + ((side + 1) % 3)]}];
        }
    }
    const DVec3 low = sloped.grid.low(page_root);
    const double size = sloped.grid.size(page_root.depth);
    const auto on_sides = [&](std::uint32_t index) {
        const DVec3 at = DVec3{one.vertices[index].position} - low;
        return glm::any(glm::lessThan(glm::abs(at), DVec3{1e-3})) ||
               glm::any(glm::lessThan(glm::abs(at - DVec3{size}), DVec3{1e-3}));
    };
    for (const auto& [edge, count] : directed) {
        CHECK(count == 1);
        if (!directed.contains({edge.second, edge.first})) {
            CHECK((on_sides(edge.first) && on_sides(edge.second)));
        }
    }
}
