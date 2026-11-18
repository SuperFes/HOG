#include <hog/client/transvoxel.hpp>
#include <hog/client/transvoxel_tables.hpp>
#include <hog/grid/grid.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <utility>

using hog::DVec3;
using hog::I64Vec3;
using hog::client::Block;
using hog::client::BlockField;
using hog::client::Crossed;
using hog::client::ShapeMesh;
namespace transvoxel = hog::client::transvoxel;

namespace {

// Of a transition cell's samples, where they lie on its full face, and which bit of its case
// each takes; those on the half face copy the full face's corners.
constexpr std::array<int, 9> sample_bit{0, 1, 2, 7, 8, 3, 6, 5, 4};
constexpr std::array<unsigned, 4> half_copies{0, 2, 6, 8};

bool solid_sample(unsigned code, unsigned sample) {
    const unsigned full = sample < 9 ? sample : half_copies[sample - 9];
    return ((code >> sample_bit[full]) & 1U) != 0U;
}

// A ball `radius` metres round `centre`, solid inside.
BlockField ball(const hog::grid::Grid& grid, DVec3 centre, double radius) {
    const DVec3 low = grid.low(hog::grid::Key{});
    const double finest = grid.finest();
    const auto at = [=](I64Vec3 point) { return low + (DVec3{point} * finest); };
    const auto value = [=](I64Vec3 point) { return glm::length(at(point) - centre) - radius; };
    return {.value = value, .crossing = [=](I64Vec3 from, I64Vec3 to) {
                const double a = value(from);
                const double b = value(to);
                const DVec3 point = glm::mix(at(from), at(to), a / (a - b));
                return Crossed{.at = point,
                               .normal = hog::Vec3{glm::normalize(point - centre)},
                               .materials = {1, 0},
                               .share = 1.0F};
            }};
}

struct Checked {
    bool closed = true;   // each edge between two triangles
    bool oriented = true; // and crossed once each way
    bool outward = true;  // facing away from the centre
    long euler = 0;
};

Checked check(const ShapeMesh& mesh, DVec3 centre) {
    Checked checked;
    std::map<std::pair<std::uint32_t, std::uint32_t>, int> directed;
    for (std::size_t at = 0; at < mesh.indices.size(); at += 3) {
        const std::array<std::uint32_t, 3> corners{mesh.indices[at], mesh.indices[at + 1],
                                                   mesh.indices[at + 2]};
        for (std::size_t side = 0; side < 3; ++side) {
            ++directed[{corners[side], corners[(side + 1) % 3]}];
        }
        const DVec3 a = mesh.origin + DVec3{mesh.vertices[corners[0]].position};
        const DVec3 b = mesh.origin + DVec3{mesh.vertices[corners[1]].position};
        const DVec3 c = mesh.origin + DVec3{mesh.vertices[corners[2]].position};
        if (glm::dot(glm::cross(b - a, c - a), ((a + b + c) / 3.0) - centre) <= 0.0) {
            checked.outward = false;
        }
    }
    long edges = 0;
    for (const auto& [edge, count] : directed) {
        const auto back = directed.find({edge.second, edge.first});
        checked.closed = checked.closed && back != directed.end();
        checked.oriented = checked.oriented && count == 1;
        edges += edge.first < edge.second ? 1 : 0;
    }
    checked.euler = static_cast<long>(mesh.vertices.size()) - edges +
                    static_cast<long>(mesh.indices.size() / 3);
    return checked;
}

} // namespace

TEST_CASE("each regular cell's vertices lie on edges between solid and open corners",
          "[client][transvoxel]") {
    for (unsigned code = 1; code < 0xFF; ++code) {
        const auto& data = transvoxel::regular_cell_data[transvoxel::regular_cell_class[code]];
        const unsigned vertices = data.counts >> 4U;
        REQUIRE(vertices > 0);
        for (unsigned index = 0; index < vertices; ++index) {
            const unsigned ends = transvoxel::regular_vertex_data[code][index] & 0xFFU;
            const unsigned a = ends >> 4U;
            const unsigned b = ends & 0xFU;
            CHECK(std::popcount(a ^ b) == 1);
            CHECK(((code >> a) & 1U) != ((code >> b) & 1U));
        }
        for (unsigned index = 0; index < 3U * (data.counts & 0xFU); ++index) {
            CHECK(data.indices[index] < vertices);
        }
    }
}

TEST_CASE("each transition cell's vertices lie on edges between solid and open samples",
          "[client][transvoxel]") {
    for (unsigned code = 1; code < 0x1FF; ++code) {
        const auto& data =
            transvoxel::transition_cell_data[transvoxel::transition_cell_class[code] & 0x7FU];
        const unsigned vertices = data.counts >> 4U;
        REQUIRE(vertices > 0);
        for (unsigned index = 0; index < vertices; ++index) {
            const unsigned ends = transvoxel::transition_vertex_data[code][index] & 0xFFU;
            const unsigned a = ends >> 4U;
            const unsigned b = ends & 0xFU;
            CHECK(solid_sample(code, a) != solid_sample(code, b));
            // Along the full face between neighbours, or round the half face.
            if (a < 9 && b < 9) {
                CHECK(std::abs(static_cast<int>(a % 3) - static_cast<int>(b % 3)) +
                          std::abs(static_cast<int>(a / 3) - static_cast<int>(b / 3)) ==
                      1);
            } else {
                CHECK((a >= 9 && b >= 9));
                CHECK(std::popcount((a - 9) ^ (b - 9)) == 1);
            }
        }
        for (unsigned index = 0; index < 3U * (data.counts & 0xFU); ++index) {
            CHECK(data.indices[index] < vertices);
        }
    }
}

TEST_CASE("a ball in a block meshes closed, facing out", "[client][transvoxel]") {
    const hog::grid::Grid grid{20.0, 1.0};
    const DVec3 centre = grid.centre(hog::grid::Key{}) + DVec3{0.3, -0.2, 0.1};
    const ShapeMesh mesh = mesh_block(grid, Block{.node = hog::grid::Key{}, .cells = 4, .finer = 0},
                                      ball(grid, centre, 0.3 * grid.size(0)));
    REQUIRE(!mesh.indices.empty());
    const Checked checked = check(mesh, centre);
    CHECK(checked.closed);
    CHECK(checked.oriented);
    CHECK(checked.outward);
    CHECK(checked.euler == 2);
}
