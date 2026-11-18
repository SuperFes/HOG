#include <hog/client/transvoxel.hpp>
#include <hog/client/transvoxel_tables.hpp>
#include <hog/grid/grid.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <optional>
#include <span>
#include <utility>
#include <vector>

using hog::DVec3;
using hog::I64Vec3;
using hog::IVec3;
using hog::client::Block;
using hog::client::BlockField;
using hog::client::Crossed;
using hog::client::ShapeMesh;
using hog::grid::Key;
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
        // Those moved flat to make room for transition cells face no way.
        const DVec3 facing = glm::cross(b - a, c - a);
        if (glm::length(facing) > 1e-9 && glm::dot(facing, ((a + b + c) / 3.0) - centre) <= 0.0) {
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

// The root's eight children drawn as blocks, those in `split` as their own eight, each block's
// finer sides where a split one is beside it.
std::vector<Block> blocks_of(std::span<const Key> split, std::uint8_t cells) {
    std::vector<Block> blocks;
    for (std::size_t octant = 0; octant < 8; ++octant) {
        const Key child = Key{}.child(octant);
        if (std::ranges::find(split, child) == split.end()) {
            blocks.push_back({.node = child, .cells = cells, .finer = 0});
            continue;
        }
        for (std::size_t inner = 0; inner < 8; ++inner) {
            blocks.push_back({.node = child.child(inner), .cells = cells, .finer = 0});
        }
    }
    for (Block& block : blocks) {
        if (block.node.depth != 1) {
            continue;
        }
        for (std::size_t axis = 0; axis < 3; ++axis) {
            for (const std::size_t side : {0U, 1U}) {
                IVec3 step{0};
                step[axis] = side == 0 ? -1 : 1;
                const auto next = block.node.beside(step.x, step.y, step.z);
                if (next && std::ranges::find(split, *next) != split.end()) {
                    block.finer =
                        static_cast<std::uint8_t>(block.finer | (1U << ((2 * axis) + side)));
                }
            }
        }
    }
    return blocks;
}

// Points welded where they lie within a millimetre.
class Welder {
public:
    std::uint32_t weld(DVec3 point) {
        const DVec3 scaled = point / tolerance;
        const std::array<long, 3> cell{std::lround(scaled.x), std::lround(scaled.y),
                                       std::lround(scaled.z)};
        for (long dx = -1; dx <= 1; ++dx) {
            for (long dy = -1; dy <= 1; ++dy) {
                for (long dz = -1; dz <= 1; ++dz) {
                    if (const auto found =
                            near({cell[0] + dx, cell[1] + dy, cell[2] + dz}, point)) {
                        return *found;
                    }
                }
            }
        }
        const auto index = static_cast<std::uint32_t>(points_.size());
        points_.push_back(point);
        cells_[cell].push_back(index);
        return index;
    }
    [[nodiscard]] std::size_t size() const noexcept { return points_.size(); }

private:
    static constexpr double tolerance = 1e-3;

    [[nodiscard]] std::optional<std::uint32_t> near(const std::array<long, 3>& cell,
                                                    DVec3 point) const {
        const auto found = cells_.find(cell);
        if (found == cells_.end()) {
            return std::nullopt;
        }
        for (const std::uint32_t index : found->second) {
            if (glm::length(points_[index] - point) < tolerance) {
                return index;
            }
        }
        return std::nullopt;
    }

    std::vector<DVec3> points_;
    std::map<std::array<long, 3>, std::vector<std::uint32_t>> cells_;
};

// The meshes as one, their vertices welded where they meet, and triangles welded down to
// nothing left out.
ShapeMesh welded(const std::vector<ShapeMesh>& meshes) {
    ShapeMesh one;
    Welder welder;
    const auto weld = [&](DVec3 point) {
        const std::uint32_t index = welder.weld(point);
        if (index == one.vertices.size()) {
            one.vertices.push_back({.position = hog::Vec3{point}});
        }
        return index;
    };
    for (const ShapeMesh& mesh : meshes) {
        std::vector<std::uint32_t> to;
        to.reserve(mesh.vertices.size());
        for (const auto& vertex : mesh.vertices) {
            to.push_back(weld(mesh.origin + DVec3{vertex.position}));
        }
        for (std::size_t at = 0; at < mesh.indices.size(); at += 3) {
            const std::uint32_t a = to[mesh.indices[at]];
            const std::uint32_t b = to[mesh.indices[at + 1]];
            const std::uint32_t c = to[mesh.indices[at + 2]];
            if (a != b && b != c && c != a) {
                one.indices.insert(one.indices.end(), {a, b, c});
            }
        }
    }
    return one;
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

TEST_CASE("blocks beside finer ones meet them closed", "[client][transvoxel]") {
    const hog::grid::Grid grid{20.0, 1.0};
    const DVec3 centre = grid.centre(Key{}) + DVec3{0.37, -0.21, 0.13};
    const BlockField field = ball(grid, centre, 0.3 * grid.size(0));
    // Finer on a low side, on a high side, and on two sides at once.
    const std::array<std::vector<Key>, 3> splits{std::vector<Key>{Key{}.child(0)},
                                                 std::vector<Key>{Key{}.child(0), Key{}.child(7)},
                                                 std::vector<Key>{Key{}.child(0), Key{}.child(3)}};
    for (const auto& split : splits) {
        CAPTURE(split.size(), split.back().x, split.back().y, split.back().z);
        std::vector<ShapeMesh> meshes;
        const std::vector<Block> blocks = blocks_of(split, 3);
        meshes.reserve(blocks.size());
        CHECK(std::ranges::any_of(blocks, [](const Block& block) { return block.finer != 0; }));
        for (const Block& block : blocks) {
            meshes.push_back(mesh_block(grid, block, field));
        }
        const Checked checked = check(welded(meshes), centre);
        CHECK(checked.closed);
        CHECK(checked.oriented);
        CHECK(checked.outward);
        CHECK(checked.euler == 2);
    }
}
