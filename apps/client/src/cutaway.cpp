#include <hog/client/cutaway.hpp>
#include <hog/globe/cell.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <numbers>
#include <unordered_map>
#include <utility>
#include <vector>

namespace hog::client {
namespace {

// Times the faces of an icosahedron are split: a face's edge is then about a thirtieth of the
// radius.
constexpr int splits = 5;

struct Ball {
    std::vector<DVec3> points;
    std::vector<std::array<std::uint32_t, 3>> faces;
};

// A sphere of unit radius as a split icosahedron.
Ball icosphere() {
    const double golden = std::numbers::phi;
    Ball ball;
    for (const DVec3 corner :
         {DVec3{-1, golden, 0}, DVec3{1, golden, 0}, DVec3{-1, -golden, 0}, DVec3{1, -golden, 0},
          DVec3{0, -1, golden}, DVec3{0, 1, golden}, DVec3{0, -1, -golden}, DVec3{0, 1, -golden},
          DVec3{golden, 0, -1}, DVec3{golden, 0, 1}, DVec3{-golden, 0, -1}, DVec3{-golden, 0, 1}}) {
        ball.points.push_back(glm::normalize(corner));
    }
    ball.faces = {{0, 11, 5}, {0, 5, 1},  {0, 1, 7},   {0, 7, 10}, {0, 10, 11},
                  {1, 5, 9},  {5, 11, 4}, {11, 10, 2}, {10, 7, 6}, {7, 1, 8},
                  {3, 9, 4},  {3, 4, 2},  {3, 2, 6},   {3, 6, 8},  {3, 8, 9},
                  {4, 9, 5},  {2, 4, 11}, {6, 2, 10},  {8, 6, 7},  {9, 8, 1}};
    for (int split = 0; split < splits; ++split) {
        std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t> middles;
        const auto middle = [&](std::uint32_t a, std::uint32_t b) {
            const auto key = std::minmax(a, b);
            const auto [found, added] =
                middles.try_emplace(key, static_cast<std::uint32_t>(ball.points.size()));
            if (added) {
                ball.points.push_back(glm::normalize(ball.points[a] + ball.points[b]));
            }
            return found->second;
        };
        std::vector<std::array<std::uint32_t, 3>> finer;
        finer.reserve(ball.faces.size() * 4);
        for (const auto& [a, b, c] : ball.faces) {
            const std::uint32_t ab = middle(a, b);
            const std::uint32_t bc = middle(b, c);
            const std::uint32_t ca = middle(c, a);
            finer.push_back({a, ab, ca});
            finer.push_back({b, bc, ab});
            finer.push_back({c, ca, bc});
            finer.push_back({ab, bc, ca});
        }
        ball.faces = std::move(finer);
    }
    return ball;
}

} // namespace

terrain::BlockMesh cutaway_shell(const globe::World& world, WorldPosition centre, double radius) {
    const globe::Layers& layers = world.store().layers();
    const Ball ball = icosphere();
    // Each brick the ball passes through, read once.
    std::map<globe::BrickSite, globe::BrickVoxels> bricks;
    std::vector<globe::Material> material(ball.points.size(), globe::Material::Air);
    terrain::BlockMesh shell{.origin = centre, .vertices = {}, .indices = {}};
    shell.vertices.reserve(ball.points.size());
    for (std::size_t k = 0; k < ball.points.size(); ++k) {
        const DVec3 point = centre.metres() + (ball.points[k] * radius);
        const double out = glm::length(point);
        const int layer = layers.layer_at(out);
        if (layer >= 0 && layer < layers.count) {
            const globe::Cell cell = globe::cell_under(point / out, layers.level);
            const globe::Site site{.face = cell.face,
                                   .i = cell.i,
                                   .j = cell.j,
                                   .layer = static_cast<std::uint32_t>(layer)};
            const globe::BrickSite brick = globe::brick_of(site);
            auto found = bricks.find(brick);
            if (found == bricks.end()) {
                found = bricks.emplace(brick, world.brick(brick)).first;
            }
            // The voxel the point is in, as it now is.
            material[k] =
                found->second[static_cast<std::size_t>(globe::index_in_brick(site))].solid;
        }
        const auto tile = static_cast<std::uint8_t>(material[k]);
        shell.vertices.push_back({.position = Vec3{ball.points[k] * radius},
                                  .normal = Vec3{-ball.points[k]},
                                  .tiles = {tile, tile, tile},
                                  .corner = terrain::packed_corner(0, 0.0, 0.0)});
    }
    for (const auto& face : ball.faces) {
        if (std::ranges::all_of(face, [&](std::uint32_t corner) {
                return material[corner] != globe::Material::Air;
            })) {
            shell.indices.insert(shell.indices.end(), face.begin(), face.end());
        }
    }
    terrain::mix_corners(shell);
    return shell;
}

} // namespace hog::client
