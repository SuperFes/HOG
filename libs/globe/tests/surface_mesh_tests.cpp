#include <hog/globe/brick.hpp>
#include <hog/globe/cell.hpp>
#include <hog/globe/detail.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/planet.hpp>
#include <hog/globe/surface_mesh.hpp>
#include <hog/globe/voxel.hpp>
#include <hog/globe/voxel_store.hpp>
#include <hog/globe/world.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstddef>
#include <cstdint>
#include <format>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <utility>
#include <vector>

using hog::DVec3;
using hog::globe::Cell;
using hog::globe::Generator;
using hog::globe::Material;
using hog::globe::SurfaceMesh;
using hog::globe::VoxelStore;
using hog::globe::World;

namespace {

const Generator& earth() {
    static const Generator generator{hog::globe::Planet{}};
    return generator;
}

const Generator& venus() {
    static const Generator generator{hog::globe::preset("venus").value_or(hog::globe::Planet{})};
    return generator;
}

const Generator& moon() {
    static const Generator generator{hog::globe::preset("moon").value_or(hog::globe::Planet{})};
    return generator;
}

DVec3 somewhere(std::uint64_t k) {
    return hog::globe::center(hog::globe::cell_at_index(7, (k * 2'654'435'761ULL) % 98'304));
}

// A direction whose ground stands between two heights over the radius, rising gently, with no
// cave within a chunk's width under it, so a chunk there holds the ground alone.
DVec3 ground_between(const Generator& generator, double low, double high) {
    const hog::globe::Layers& layers = generator.layers();
    for (std::uint64_t k = 0;; ++k) {
        const DVec3 direction = somewhere(k);
        const double ground = hog::globe::ground_below(generator, direction, 0.5).value_or(0.0) -
                              generator.planet().radius;
        const DVec3 east = glm::normalize(glm::cross(DVec3{0.0, 0.0, 1.0}, direction));
        const DVec3 north = glm::cross(direction, east);
        bool gentle = true;
        for (const DVec3 side : {east, -east, north, -north}) {
            const DVec3 across =
                glm::normalize(direction + (side * 24.0 / generator.planet().radius));
            gentle =
                gentle && std::abs(hog::globe::ground_below(generator, across, 0.5).value_or(0.0) -
                                   generator.planet().radius - ground) < 8.0;
        }
        if (gentle && ground > low && ground < high &&
            generator.column(direction, 0.5)
                .caves(layers.bottom(0), layers.top(layers.count - 1), 16.0)
                .empty()) {
            return direction;
        }
    }
}

SurfaceMesh mesh_of(const World& world, std::span<const Cell> chunks,
                    std::optional<hog::globe::Sight> sight = std::nullopt) {
    SurfaceMesh mesh;
    mesh.origin = hog::globe::center(chunks.front()) * world.generator().planet().radius;
    for (const Cell chunk : chunks) {
        hog::globe::add_chunk(mesh, world, chunk, hog::globe::Drawn{}, sight);
    }
    return mesh;
}

bool liquid(Material material) {
    return material == Material::Water;
}

DVec3 world_point(const SurfaceMesh& mesh, std::uint32_t vertex) {
    return mesh.origin + DVec3{mesh.vertices[vertex].position};
}

// Sides of triangles that no other triangle shares, by where they lie: vertices are matched by
// position to a tenth of a millimetre, as chunks each find their own.
std::vector<DVec3> open_sides(const SurfaceMesh& mesh) {
    std::map<std::array<long long, 3>, std::uint32_t> welded;
    std::vector<std::uint32_t> same(mesh.vertices.size());
    for (std::uint32_t v = 0; v < mesh.vertices.size(); ++v) {
        const DVec3 at = DVec3{mesh.vertices[v].position} * 1e4;
        const std::array key{std::llround(at.x), std::llround(at.y), std::llround(at.z)};
        same[v] = welded.try_emplace(key, v).first->second;
    }
    std::map<std::pair<std::uint32_t, std::uint32_t>, int> uses;
    for (std::size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
        for (std::size_t k = 0; k < 3; ++k) {
            const std::uint32_t a = same[mesh.indices[t + k]];
            const std::uint32_t b = same[mesh.indices[t + ((k + 1) % 3)]];
            ++uses[std::minmax(a, b)];
        }
    }
    std::vector<DVec3> open;
    for (const auto& [side, count] : uses) {
        if (count == 1) {
            open.push_back(0.5 * (world_point(mesh, side.first) + world_point(mesh, side.second)));
        }
    }
    return open;
}

// Changes each open voxel within `radius` of `centre`, or every voxel there with `ground` too,
// given it and the radius its bottom stands at.
void fill_hollow(World& world, DVec3 centre, double radius, const auto& change,
                 bool ground = false) {
    const hog::globe::Layers& layers = world.generator().layers();
    const Cell middle = hog::globe::cell_under(glm::normalize(centre), layers.level);
    const int layer = layers.layer_at(glm::length(centre));
    const int reach = static_cast<int>(std::ceil(radius / layers.thickness)) + 1;
    std::map<hog::globe::BrickSite, hog::globe::BrickVoxels> bricks;
    for (int dl = -reach; dl <= reach; ++dl) {
        for (int dj = -reach; dj <= reach; ++dj) {
            for (int di = -reach; di <= reach; ++di) {
                const std::optional<Cell> cell = hog::globe::next_to(middle, di, dj);
                if (!cell) {
                    continue;
                }
                const hog::globe::Site site{.face = cell->face,
                                            .i = cell->i,
                                            .j = cell->j,
                                            .layer = static_cast<std::uint32_t>(layer + dl)};
                if (glm::length(hog::globe::middle_of(site, layers) - centre) > radius) {
                    continue;
                }
                const hog::globe::BrickSite brick = hog::globe::brick_of(site);
                auto held = bricks.find(brick);
                if (held == bricks.end()) {
                    held = bricks.emplace(brick, world.brick(brick)).first;
                }
                hog::globe::Voxel& voxel =
                    held->second[static_cast<std::size_t>(hog::globe::index_in_brick(site))];
                if (ground || voxel.solid == Material::Air) {
                    change(voxel, layers.bottom(static_cast<int>(site.layer)));
                }
            }
        }
    }
    for (const auto& [brick, voxels] : bricks) {
        world.write(brick, voxels);
    }
}

// A hollow dug in the ground, and its centre.
DVec3 dig_hollow(World& world, DVec3 up, double radius) {
    const double ground = hog::globe::ground_below(earth(), up, 0.5).value_or(0.0);
    hog::globe::dig(world, up * ground, radius);
    return up * ground;
}

// Vertices of `material` within `across` metres of `centre` sideways.
std::vector<std::uint32_t> vertices_near(const SurfaceMesh& mesh, DVec3 centre, double across,
                                         Material material) {
    const DVec3 up = glm::normalize(centre);
    std::vector<std::uint32_t> found;
    for (std::uint32_t v = 0; v < mesh.vertices.size(); ++v) {
        const DVec3 out = world_point(mesh, v) - centre;
        if (mesh.vertices[v].material == material &&
            glm::length(out - (glm::dot(out, up) * up)) <= across) {
            found.push_back(v);
        }
    }
    return found;
}

// The chunk and the eight round it, or seven at the cube's corners.
std::vector<Cell> block_round(Cell chunk) {
    std::vector<Cell> block{chunk};
    const hog::globe::CellNeighbors near = hog::globe::around(chunk);
    block.insert(block.end(), near.all().begin(), near.all().end());
    return block;
}

// Open sides that lie in the chunk, above where caves stop being drawn.
std::size_t open_in(const World& world, const SurfaceMesh& mesh, Cell chunk) {
    const hog::globe::Layers& layers = world.generator().layers();
    const int detail = hog::globe::detail_of(chunk, layers);
    const int apart = hog::globe::apart_at(detail);
    const double depth = std::max(hog::globe::surface_depth, distance_band(layers, apart)) -
                         (4.0 * apart * layers.thickness);
    std::size_t count = 0;
    for (const DVec3 side : open_sides(mesh)) {
        const DVec3 up = glm::normalize(side);
        const double ground = hog::globe::ground_below(world.generator(), up, 0.5).value_or(0.0);
        if (hog::globe::chunk_under(up, layers, detail) == chunk &&
            glm::length(side) > ground - depth) {
            ++count;
        }
    }
    return count;
}

// A chunk over land with a cave well under its lowest ground, past where the ground alone is
// drawn to, and the radii the cave lies between.
std::optional<std::pair<Cell, std::pair<double, double>>> chunk_with_deep_cave() {
    const hog::globe::Layers& layers = earth().layers();
    const double lod = hog::globe::voxel_lod(earth());
    for (std::uint64_t k = 0; k < 4'000; ++k) {
        const DVec3 direction = somewhere(k);
        const Generator::Column column = earth().column(direction, lod);
        if (column.ground() < earth().sea_level().value_or(0.0) + 20.0) {
            continue;
        }
        for (const auto& found : column.caves(column.ground() - 150.0, column.ground(), 0.0)) {
            // Cast for with no margin, a stretch may still hold no cave: one that does.
            bool open = false;
            const int steps = static_cast<int>((found.second - found.first) / 0.25);
            for (int step = 0; step <= steps && !open; ++step) {
                open =
                    column.sample(found.first + (static_cast<double>(step) * 0.25)).distance > 0.0;
            }
            if (open && found.second < column.ground() - hog::globe::surface_depth - 10.0) {
                return std::pair{hog::globe::chunk_under(direction, layers), found};
            }
        }
    }
    return std::nullopt;
}

} // namespace

TEST_CASE("a chunk's surface lies on the ground, facing out of it", "[globe][surface]") {
    VoxelStore store{earth().layers()};
    const World world{earth(), store};
    const DVec3 up = ground_between(earth(), 50.0, 400.0);
    const Cell chunk = hog::globe::chunk_under(up, earth().layers());
    const SurfaceMesh mesh = mesh_of(world, std::array{chunk});
    REQUIRE(mesh.vertices.size() > 500);
    REQUIRE(mesh.indices.size() % 3 == 0);
    double off = 0.0;
    double worst = 0.0;
    std::size_t facing = 0;
    for (std::uint32_t v = 0; v < mesh.vertices.size(); ++v) {
        CHECK_FALSE(liquid(mesh.vertices[v].material));
        CHECK(mesh.vertices[v].material != Material::Air);
        const DVec3 point = world_point(mesh, v);
        const double distance =
            std::abs(earth().sample(point, hog::globe::voxel_lod(earth())).distance);
        off += distance;
        worst = std::max(worst, distance);
        facing += glm::dot(DVec3{mesh.vertices[v].normal}, glm::normalize(point)) > 0.3 ? 1U : 0U;
    }
    // The ground between voxels is drawn as the straight line between their distances.
    CHECK(off / static_cast<double>(mesh.vertices.size()) < 0.05);
    CHECK(worst < 0.3);
    CHECK(facing > mesh.vertices.size() * 99 / 100);
    // Triangles turn their fronts out of the ground, but for slivers where a quad folds.
    std::size_t backwards = 0;
    for (std::size_t t = 0; t < mesh.indices.size(); t += 3) {
        const DVec3 a = DVec3{mesh.vertices[mesh.indices[t]].position};
        const DVec3 b = DVec3{mesh.vertices[mesh.indices[t + 1]].position};
        const DVec3 c = DVec3{mesh.vertices[mesh.indices[t + 2]].position};
        const DVec3 normal = DVec3{mesh.vertices[mesh.indices[t]].normal} +
                             DVec3{mesh.vertices[mesh.indices[t + 1]].normal} +
                             DVec3{mesh.vertices[mesh.indices[t + 2]].normal};
        backwards += glm::dot(glm::cross(b - a, c - a), normal) < 0.0 ? 1U : 0U;
    }
    CHECK(backwards * 1'000 < mesh.indices.size() / 3);
}

TEST_CASE("a surface drawn from solid amounts alone lies near the ground and faces out of it",
          "[globe][surface][amounts]") {
    VoxelStore store{earth().layers()};
    World world{earth(), store};
    world.draw_from_amounts(true);
    const DVec3 up = ground_between(earth(), 50.0, 400.0);
    const Cell chunk = hog::globe::chunk_under(up, earth().layers());
    const SurfaceMesh mesh = mesh_of(world, std::array{chunk});
    REQUIRE(mesh.vertices.size() > 500);
    double off = 0.0;
    double worst = 0.0;
    std::size_t facing = 0;
    for (std::uint32_t v = 0; v < mesh.vertices.size(); ++v) {
        const DVec3 point = world_point(mesh, v);
        const double distance =
            std::abs(earth().sample(point, hog::globe::voxel_lod(earth())).distance);
        off += distance;
        worst = std::max(worst, distance);
        facing += glm::dot(DVec3{mesh.vertices[v].normal}, glm::normalize(point)) > 0.3 ? 1U : 0U;
    }
    std::printf("amounts: mean %.4f m off, worst %.4f m, %zu of %zu facing out\n",
                off / static_cast<double>(mesh.vertices.size()), worst, facing,
                mesh.vertices.size());
    // Within a fraction of a voxel's thickness, which is all a voxel's amount can tell.
    const double thickness = earth().layers().thickness;
    CHECK(off / static_cast<double>(mesh.vertices.size()) < 0.15 * thickness);
    CHECK(worst < 0.5 * thickness);
    CHECK(facing > mesh.vertices.size() * 95 / 100);
}

TEST_CASE("chunks of a detail meet without gaps, on a face and across the cube's edges",
          "[globe][surface]") {
    VoxelStore store{moon().layers()};
    const World world{moon(), store};
    const hog::globe::Layers& layers = moon().layers();
    const int detail = GENERATE(0, 1, 3);
    const int level = layers.level - hog::globe::chunk_levels - detail;
    const auto last = (std::uint32_t{1} << level) - 1;
    // In a face's middle, and across edges whose faces' cells run up to them from both sides,
    // down to them from both, and up to one and down to the other.
    for (const DVec3 up :
         {somewhere(11), glm::normalize(DVec3{1.0, 1.0, 0.2}),
          glm::normalize(DVec3{-0.3, 1.0, -1.0}), glm::normalize(DVec3{-1.0, -1.0, 0.3})}) {
        const Cell chunk = hog::globe::chunk_under(up, layers, detail);
        CAPTURE(detail, chunk.face, chunk.i, chunk.j);
        const std::vector<Cell> block = block_round(chunk);
        const SurfaceMesh mesh = mesh_of(world, block);
        REQUIRE(mesh.vertices.size() > 1'000);
        CHECK(open_in(world, mesh, chunk) == 0);
        std::set<int> faces;
        for (const Cell cell : block) {
            faces.insert(cell.face);
        }
        CHECK(faces.size() ==
              ((chunk.i == 0 || chunk.i == last || chunk.j == 0 || chunk.j == last) ? 2U : 1U));
    }
}

namespace {

// Meshes the chunks round an eye standing at `up` on Venus, so the details change within a few
// chunks of it, and checks they leave no gaps.
void check_details_meet(DVec3 up, double reach, double ratio) {
    VoxelStore store{venus().layers()};
    const World world{venus(), store};
    const hog::globe::Layers& layers = venus().layers();
    const std::vector<Cell> chunks = hog::globe::chunks_around(venus(), {.up = up}, reach, ratio);
    const hog::globe::Drawn drawn{chunks};
    SurfaceMesh mesh;
    mesh.origin = up * venus().planet().radius;
    std::set<int> details;
    std::set<int> faces;
    for (const Cell chunk : chunks) {
        hog::globe::add_chunk(mesh, world, chunk, drawn);
        details.insert(hog::globe::detail_of(chunk, layers));
        faces.insert(chunk.face);
    }
    CAPTURE(up.x, up.y, up.z, chunks.size(), details.size(), faces.size());
    // At the cube's corner, three faces' chunks at one detail leave room for fewer.
    REQUIRE(details.size() >= (faces.size() == 3 ? 2U : 3U));
    // Open sides only round the drawn chunks' rim, and under where caves stop being drawn.
    std::size_t open = 0;
    for (const DVec3 side : open_sides(mesh)) {
        const DVec3 dir = glm::normalize(side);
        const std::optional<Cell> chunk =
            drawn.over(hog::globe::cell_under(dir, layers.level - hog::globe::chunk_levels));
        if (!chunk) {
            continue;
        }
        const hog::globe::CellNeighbors near = hog::globe::around(*chunk);
        const bool rim = std::ranges::any_of(near.all(), [&](Cell beside) {
            return !drawn.over(beside).has_value() &&
                   !std::ranges::any_of(hog::globe::children(beside),
                                        [&](Cell finer) { return drawn.contains(finer); });
        });
        const int apart = hog::globe::apart_at(hog::globe::detail_of(*chunk, layers));
        const double depth = std::max(hog::globe::surface_depth, distance_band(layers, apart)) -
                             (4.0 * apart * layers.thickness);
        const double ground = hog::globe::ground_below(venus(), dir, 0.5).value_or(0.0);
        if (!rim && glm::length(side) > ground - depth) {
            ++open;
            UNSCOPED_INFO("open at detail " << hog::globe::detail_of(*chunk, layers) << " face "
                                            << int{chunk->face} << " " << glm::length(side) - ground
                                            << " m over");
        }
    }
    CHECK(open == 0);
}

} // namespace

TEST_CASE("chunks at other details meet without gaps, across a cube's edge", "[globe][surface]") {
    // The fewest chunks that still hold three details across an edge.
    check_details_meet(glm::normalize(DVec3{-1.0, -1.0, 0.3}), 70.0, 1.01);
}

// Each spot costs a minute in a debug build; run by hand on a release build:
// hog_globe_tests "[.gate]".
TEST_CASE("chunks at other details meet without gaps, at every kind of edge and a corner",
          "[globe][surface][.gate]") {
    for (const DVec3 up :
         {somewhere(11), glm::normalize(DVec3{1.0, 1.0, 0.2}),
          glm::normalize(DVec3{-0.3, 1.0, -1.0}), glm::normalize(DVec3{-1.0, -1.0, 0.3}),
          glm::normalize(DVec3{1.0, 1.0, 1.0})}) {
        check_details_meet(up, 100.0, 1.1);
    }
}

TEST_CASE("coarser chunks lie on their coarser ground, as near for their size",
          "[globe][surface]") {
    VoxelStore store{earth().layers()};
    const World world{earth(), store};
    const hog::globe::Layers& layers = earth().layers();
    const DVec3 up = ground_between(earth(), 50.0, 400.0);
    for (const int detail : {1, 3, 5}) {
        const int apart = hog::globe::apart_at(detail);
        const double lod = hog::globe::voxel_lod(earth()) * apart;
        const SurfaceMesh mesh =
            mesh_of(world, std::array{hog::globe::chunk_under(up, layers, detail)});
        REQUIRE(mesh.vertices.size() > 500);
        double off = 0.0;
        double worst = 0.0;
        double off_fine = 0.0;
        // Of those on the ground, rather than in caves under it.
        std::size_t on_ground = 0;
        std::size_t facing = 0;
        for (std::uint32_t v = 0; v < mesh.vertices.size(); ++v) {
            const DVec3 point = world_point(mesh, v);
            const double distance = std::abs(earth().sample(point, lod).distance);
            off += distance;
            worst = std::max(worst, distance);
            off_fine += std::abs(earth().sample(point, hog::globe::voxel_lod(earth())).distance);
            const DVec3 up_there = glm::normalize(point);
            const double ground = hog::globe::ground_below(earth(), up_there, lod).value_or(0.0);
            if (std::abs(glm::length(point) - ground) < 2.0 * apart * layers.thickness) {
                ++on_ground;
                facing += glm::dot(DVec3{mesh.vertices[v].normal}, up_there) > 0.3 ? 1U : 0U;
            }
        }
        const auto count = static_cast<double>(mesh.vertices.size());
        CAPTURE(detail, off / count, worst, off_fine / count, on_ground, facing);
        CHECK(off / count < 0.05 * apart);
        CHECK(worst < 0.3 * apart);
        // Off the finest ground by about what's smoothed away.
        CHECK(off_fine / count < 0.5 * apart);
        CHECK(on_ground > mesh.vertices.size() / 2);
        CHECK(facing > on_ground * 99 / 100);
    }
}

TEST_CASE("chunks meet without gaps at the cube's corners, where three columns meet",
          "[globe][surface]") {
    VoxelStore store{moon().layers()};
    const World world{moon(), store};
    const hog::globe::Layers& layers = moon().layers();
    for (const DVec3 corner : {DVec3{1.0, 1.0, 1.0}, DVec3{-1.0, -1.0, -1.0}, DVec3{1.0, -1.0, 1.0},
                               DVec3{-1.0, 1.0, -1.0}}) {
        const Cell chunk = hog::globe::chunk_under(glm::normalize(corner), layers);
        CAPTURE(corner.x, corner.y, corner.z);
        REQUIRE(block_round(chunk).size() == 8);
        const SurfaceMesh mesh = mesh_of(world, block_round(chunk));
        CHECK(open_in(world, mesh, chunk) == 0);
    }
}

TEST_CASE("a ball dug in the ground shows as a hollow", "[globe][surface]") {
    VoxelStore store{earth().layers()};
    World world{earth(), store};
    const DVec3 up = ground_between(earth(), 50.0, 400.0);
    const double ground = hog::globe::ground_below(earth(), up, 0.5).value_or(0.0);
    const Cell chunk = hog::globe::chunk_under(up, earth().layers());
    const auto deepest = [&] {
        const SurfaceMesh mesh = mesh_of(world, std::array{chunk});
        double lowest = 1e12;
        for (std::uint32_t v = 0; v < mesh.vertices.size(); ++v) {
            const DVec3 point = world_point(mesh, v);
            if (glm::length(glm::normalize(point) - up) * ground < 1.0) {
                lowest = std::min(lowest, glm::length(point));
            }
        }
        return lowest;
    };
    CHECK(deepest() > ground - 0.5);
    hog::globe::dig(world, up * ground, 3.0);
    CHECK(deepest() < ground - 2.5);
}

TEST_CASE("a hollow dug in the ground keeps a sharp lip", "[globe][surface]") {
    VoxelStore store{earth().layers()};
    World world{earth(), store};
    const DVec3 up = ground_between(earth(), 50.0, 400.0);
    const double lod = hog::globe::voxel_lod(earth());
    const DVec3 centre = up * hog::globe::ground_below(earth(), up, lod).value_or(0.0);
    constexpr double radius = 3.0;
    hog::globe::dig(world, centre, radius);
    const SurfaceMesh mesh =
        mesh_of(world, block_round(hog::globe::chunk_under(up, earth().layers())));
    // How far each vertex lies off the ground with the ball taken out of it, near where the two
    // meet: a crease all the way round.
    double worst = 0.0;
    double total = 0.0;
    int near_lip = 0;
    for (std::uint32_t v = 0; v < mesh.vertices.size(); ++v) {
        const DVec3 point = world_point(mesh, v);
        const DVec3 out = point - centre;
        const double height = glm::dot(out, up);
        const double across = glm::length(out - (height * up));
        if (std::hypot(across - radius, height) > 0.75) {
            continue;
        }
        const double ground =
            glm::length(point) -
            hog::globe::ground_below(earth(), glm::normalize(point), lod).value_or(0.0);
        const double off = std::abs(std::max(ground, radius - glm::length(out)));
        worst = std::max(worst, off);
        total += off;
        ++near_lip;
    }
    REQUIRE(near_lip > 50);
    // Surface nets round it off by 16 cm; with normals estimated from distances, 13 cm.
    CHECK(worst < 0.13);
    CHECK(total / near_lip < 0.04);
}

TEST_CASE("a cave under a chunk is drawn however deep it lies, whole and without gaps",
          "[globe][surface]") {
    VoxelStore store{earth().layers()};
    const World world{earth(), store};
    const hog::globe::Layers& layers = earth().layers();
    const double lod = hog::globe::voxel_lod(earth());
    const auto [chunk, cave] =
        chunk_with_deep_cave().value_or(std::pair{Cell{}, std::pair{0.0, 0.0}});
    REQUIRE(chunk.is_valid());
    INFO("chunk " << chunk.face << " " << chunk.i << " " << chunk.j << ", cave from " << cave.first
                  << " to " << cave.second);
    // Walls of the cave's, in the chunk, under a sight of radii.
    const auto walls_seen = [&](hog::globe::Sight sight) {
        const SurfaceMesh mesh = mesh_of(world, block_round(chunk), sight);
        std::size_t walls = 0;
        for (std::uint32_t v = 0; v < mesh.vertices.size(); ++v) {
            const double radius = glm::length(world_point(mesh, v));
            if (radius >= cave.first && radius <= cave.second &&
                hog::globe::chunk_under(glm::normalize(world_point(mesh, v)), layers) == chunk) {
                ++walls;
            }
        }
        return walls;
    };
    // Seen from where it lies, and not from far over it, where rock hides it.
    CHECK(walls_seen({.lowest = cave.first - 50.0, .highest = cave.second + 50.0}) > 20);
    CHECK(walls_seen({.lowest = cave.second + 100.0, .highest = cave.second + 400.0}) == 0);
    CHECK(walls_seen(hog::globe::sight_of(0)) == 0);
    const SurfaceMesh mesh = mesh_of(world, block_round(chunk));
    // The cave's walls are there, and every side of the chunk's triangles is shared.
    std::size_t walls = 0;
    for (std::uint32_t v = 0; v < mesh.vertices.size(); ++v) {
        const double radius = glm::length(world_point(mesh, v));
        if (radius >= cave.first && radius <= cave.second &&
            hog::globe::chunk_under(glm::normalize(world_point(mesh, v)), layers) == chunk) {
            ++walls;
        }
    }
    CHECK(walls > 20);
    std::size_t open = 0;
    std::string where;
    for (const DVec3 side : open_sides(mesh)) {
        if (hog::globe::chunk_under(glm::normalize(side), layers) == chunk) {
            ++open;
            const double ground =
                hog::globe::ground_below(earth(), glm::normalize(side), lod).value_or(0.0);
            where += std::format("{:.1f} ", ground - glm::length(side));
        }
    }
    INFO("open sides, metres under the ground: " << where);
    CHECK(open == 0);
}

TEST_CASE("a sight made for a band holds while the eye stays near it", "[globe][surface]") {
    const double radius = 114'830.0;
    const std::uint64_t band = hog::globe::sight_band(radius);
    REQUIRE(band != 0);
    const hog::globe::Sight sight = hog::globe::sight_of(band);
    CHECK(sight.lowest <= radius - hog::globe::sight_reach);
    CHECK(sight.highest >= radius + hog::globe::sight_reach);
    const double bottom = static_cast<double>(band) * hog::globe::sight_step;
    // It's kept a band either way, and the eye there still reaches half the sight's reach.
    CHECK(hog::globe::sight_holds(band, bottom - hog::globe::sight_step));
    CHECK(hog::globe::sight_holds(band, bottom + (2.0 * hog::globe::sight_step) - 1.0));
    CHECK_FALSE(hog::globe::sight_holds(band, bottom - hog::globe::sight_step - 1.0));
    CHECK_FALSE(hog::globe::sight_holds(band, bottom + (2.0 * hog::globe::sight_step)));
    CHECK_FALSE(hog::globe::sight_holds(0, radius));
    for (const double held :
         {bottom - hog::globe::sight_step, bottom + (2.0 * hog::globe::sight_step) - 1.0}) {
        const double left = hog::globe::sight_reach - hog::globe::sight_step;
        CHECK(left >= 30.0);
        CHECK(held - left >= sight.lowest);
        CHECK(held + left <= sight.highest);
    }
    // No sight at all sees nothing.
    CHECK(hog::globe::sight_of(0).lowest > hog::globe::sight_of(0).highest);
}

TEST_CASE("the sea shows as a level surface over the seabed", "[globe][surface]") {
    VoxelStore store{earth().layers()};
    const World world{earth(), store};
    const double sea = earth().sea_level().value_or(0.0);
    const DVec3 up = ground_between(earth(), -300.0, -20.0);
    const Cell chunk = hog::globe::chunk_under(up, earth().layers());
    const SurfaceMesh mesh = mesh_of(world, block_round(chunk));
    std::size_t water = 0;
    std::size_t bed = 0;
    for (std::uint32_t v = 0; v < mesh.vertices.size(); ++v) {
        const DVec3 point = world_point(mesh, v);
        if (liquid(mesh.vertices[v].material)) {
            ++water;
            CHECK(std::abs(glm::length(point) - sea) <= earth().layers().thickness);
            CHECK(glm::dot(DVec3{mesh.vertices[v].normal}, glm::normalize(point)) > 0.99);
        } else if (glm::length(point) < sea - 10.0) {
            ++bed;
        }
    }
    CHECK(water > 1'000);
    CHECK(bed > 1'000);
    CHECK(open_in(world, mesh, chunk) == 0);
}

TEST_CASE("water standing in a hollow shows level at its surface, however full its top voxel",
          "[globe][surface]") {
    VoxelStore store{earth().layers()};
    World world{earth(), store};
    const DVec3 up = ground_between(earth(), 50.0, 400.0);
    const DVec3 centre = dig_hollow(world, up, 3.0);
    const double thickness = earth().layers().thickness;
    const double level = glm::length(centre) - 1.0;
    fill_hollow(world, centre, 3.0, [&](hog::globe::Voxel& voxel, double bottom) {
        if (bottom < level) {
            voxel.liquid = Material::Water;
            voxel.liquid_amount =
                hog::globe::amount_of(std::min(1.0, (level - bottom) / thickness));
        }
    });
    const SurfaceMesh mesh =
        mesh_of(world, block_round(hog::globe::chunk_under(up, earth().layers())));
    const std::vector<std::uint32_t> water = vertices_near(mesh, centre, 1.5, Material::Water);
    CHECK(water.size() > 20);
    for (const std::uint32_t v : water) {
        CHECK(std::abs(glm::length(world_point(mesh, v)) - level) < 0.03);
        CHECK(glm::dot(DVec3{mesh.vertices[v].normal}, up) > 0.99);
    }
}

TEST_CASE("lava standing in a hollow shines by its heat, and cold rock does not",
          "[globe][surface]") {
    VoxelStore store{earth().layers()};
    World world{earth(), store};
    const DVec3 up = ground_between(earth(), 50.0, 400.0);
    const DVec3 centre = dig_hollow(world, up, 3.0);
    const double thickness = earth().layers().thickness;
    const double level = glm::length(centre) - 1.0;
    fill_hollow(world, centre, 3.0, [&](hog::globe::Voxel& voxel, double bottom) {
        if (bottom < level) {
            voxel.liquid = Material::Basalt;
            voxel.liquid_amount =
                hog::globe::amount_of(std::min(1.0, (level - bottom) / thickness));
            voxel.temperature = hog::globe::temperature_in_eighths(1'600.0);
        }
    });
    const SurfaceMesh mesh =
        mesh_of(world, block_round(hog::globe::chunk_under(up, earth().layers())));
    const std::vector<std::uint32_t> lava = vertices_near(mesh, centre, 1.5, Material::Basalt);
    REQUIRE(lava.size() > 20);
    bool shines = false;
    for (const std::uint32_t v : lava) {
        shines = shines || mesh.vertices[v].glow > 0.9F;
    }
    CHECK(shines);
    // Ground away from it is cold.
    for (const hog::globe::SurfaceVertex& vertex : mesh.vertices) {
        if (glm::length(world_point(mesh, static_cast<std::uint32_t>(&vertex - mesh.vertices.data())) -
                        centre) > 30.0) {
            CHECK(vertex.glow == 0.0F);
        }
    }
}

TEST_CASE("a film of water shallower than half a voxel still shows on the ground",
          "[globe][surface]") {
    VoxelStore store{earth().layers()};
    World world{earth(), store};
    const DVec3 up = ground_between(earth(), 50.0, 400.0);
    const DVec3 centre = dig_hollow(world, up, 3.0);
    const double thickness = earth().layers().thickness;
    const double level = glm::length(centre) - 3.0 + 0.4;
    fill_hollow(world, centre, 3.0, [&](hog::globe::Voxel& voxel, double bottom) {
        if (bottom < level) {
            voxel.liquid = Material::Water;
            voxel.liquid_amount =
                hog::globe::amount_of(std::min(1.0, (level - bottom) / thickness));
        }
    });
    const SurfaceMesh mesh =
        mesh_of(world, block_round(hog::globe::chunk_under(up, earth().layers())));
    const std::vector<std::uint32_t> water = vertices_near(mesh, centre, 2.0, Material::Water);
    CHECK(water.size() > 5);
    for (const std::uint32_t v : water) {
        CHECK(std::abs(glm::length(world_point(mesh, v)) - level) < 0.06);
    }
}

TEST_CASE("grain piled in a hollow shows as ground of its own, as high as it fills",
          "[globe][surface]") {
    VoxelStore store{earth().layers()};
    World world{earth(), store};
    const DVec3 up = ground_between(earth(), 50.0, 400.0);
    const DVec3 centre = dig_hollow(world, up, 3.0);
    const double thickness = earth().layers().thickness;
    const double level = glm::length(centre) - 1.5;
    fill_hollow(world, centre, 3.0, [&](hog::globe::Voxel& voxel, double bottom) {
        if (bottom < level) {
            voxel.grain = Material::Sand;
            voxel.grain_amount = hog::globe::amount_of(std::min(1.0, (level - bottom) / thickness));
        }
    });
    const SurfaceMesh mesh =
        mesh_of(world, block_round(hog::globe::chunk_under(up, earth().layers())));
    const std::vector<std::uint32_t> sand = vertices_near(mesh, centre, 1.0, Material::Sand);
    CHECK(sand.size() > 10);
    for (const std::uint32_t v : sand) {
        CHECK(std::abs(glm::length(world_point(mesh, v)) - level) < 0.05);
        CHECK(glm::dot(DVec3{mesh.vertices[v].normal}, up) > 0.99);
    }
    // Nothing but the sand's top is drawn over the pile.
    for (std::uint32_t v = 0; v < mesh.vertices.size(); ++v) {
        const DVec3 out = world_point(mesh, v) - centre;
        const double height = glm::dot(out, up);
        if (glm::length(out - (height * up)) <= 1.0 && height < 0.0) {
            CHECK(mesh.vertices[v].material == Material::Sand);
            CHECK(glm::length(world_point(mesh, v)) > level - 0.05);
        }
    }
}

TEST_CASE("the seabed is drawn soaked and the land dry", "[globe][surface]") {
    VoxelStore store{earth().layers()};
    const World world{earth(), store};
    const auto wetness = [&](double low, double high) {
        const DVec3 up = ground_between(earth(), low, high);
        const SurfaceMesh mesh =
            mesh_of(world, std::array{hog::globe::chunk_under(up, earth().layers())});
        double total = 0.0;
        std::size_t ground = 0;
        for (const hog::globe::SurfaceVertex& vertex : mesh.vertices) {
            if (!liquid(vertex.material)) {
                total += double{vertex.wet};
                ++ground;
            }
        }
        REQUIRE(ground > 1'000);
        return total / static_cast<double>(ground);
    };
    CHECK(wetness(-300.0, -20.0) > 0.9);
    CHECK(wetness(50.0, 400.0) == 0.0);
}

TEST_CASE("ground soaked to its pores shows wet at its surface", "[globe][surface]") {
    VoxelStore store{earth().layers()};
    World world{earth(), store};
    const DVec3 up = ground_between(earth(), 50.0, 400.0);
    const DVec3 centre = dig_hollow(world, up, 3.0);
    fill_hollow(
        world, centre, 4.0,
        [&](hog::globe::Voxel& voxel, double /*bottom*/) {
            if (voxel.solid != Material::Air) {
                voxel.liquid = Material::Water;
                voxel.liquid_amount =
                    hog::globe::amount_of(hog::globe::properties(voxel.solid).porosity);
            }
        },
        true);
    const SurfaceMesh mesh =
        mesh_of(world, block_round(hog::globe::chunk_under(up, earth().layers())));
    std::size_t near = 0;
    std::size_t far = 0;
    for (std::uint32_t v = 0; v < mesh.vertices.size(); ++v) {
        const hog::globe::SurfaceVertex& vertex = mesh.vertices[v];
        const double apart = glm::length(world_point(mesh, v) - centre);
        if (liquid(vertex.material) || (apart > 3.3 && apart < 12.0)) {
            continue;
        }
        if (apart <= 3.3) {
            CHECK(vertex.wet > 0.99F);
            ++near;
        } else {
            CHECK(vertex.wet == 0.0F);
            ++far;
        }
    }
    CHECK(near > 50);
    CHECK(far > 1'000);
}

TEST_CASE("chunks near a place are all those within reach, nearest first", "[globe][surface]") {
    VoxelStore store{earth().layers()};
    const World world{earth(), store};
    const double radius = earth().planet().radius;
    for (const DVec3 up : {somewhere(3), glm::normalize(DVec3{1.0, 1.0, 1.0})}) {
        constexpr double reach = 150.0;
        const std::vector<Cell> near = hog::globe::chunks_near(world, up, reach);
        REQUIRE_FALSE(near.empty());
        const std::set<Cell> listed{near.begin(), near.end()};
        CHECK(listed.contains(hog::globe::chunk_under(up, earth().layers())));
        CHECK(listed.size() == near.size());
        double last = 0.0;
        for (const Cell chunk : near) {
            const double apart = glm::length(hog::globe::center(chunk) - up) * radius;
            CHECK(apart <= reach);
            CHECK(apart >= last);
            last = apart;
            const hog::globe::CellNeighbors beside = hog::globe::sides(chunk);
            for (const Cell next : beside.all()) {
                if (glm::length(hog::globe::center(next) - up) * radius <= reach) {
                    CHECK(listed.contains(next));
                }
            }
        }
    }
}

TEST_CASE("chunks mesh from the world", "[.bench][globe][surface]") {
    for (const char* name : {"earth", "mars", "venus", "moon"}) {
        const Generator generator{hog::globe::preset(name).value_or(hog::globe::Planet{})};
        VoxelStore store{generator.layers()};
        const World world{generator, store};
        std::vector<Cell> chunks;
        chunks.reserve(8);
        for (std::uint64_t k = 0; k < 8; ++k) {
            chunks.push_back(hog::globe::chunk_under(somewhere(k * 97), generator.layers()));
        }
        const auto started = std::chrono::steady_clock::now();
        const SurfaceMesh mesh = mesh_of(world, chunks);
        const double seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        WARN(name << ": " << 1'000.0 * seconds / static_cast<double>(chunks.size())
                  << " ms a chunk, " << mesh.vertices.size() / chunks.size() << " vertices");
    }
}

TEST_CASE("coarser chunks mesh from the world", "[.bench][globe][surface]") {
    VoxelStore store{earth().layers()};
    const World world{earth(), store};
    for (int detail = 0; detail <= 8; ++detail) {
        std::vector<Cell> chunks;
        chunks.reserve(4);
        for (std::uint64_t k = 0; k < 4; ++k) {
            chunks.push_back(hog::globe::chunk_under(somewhere(k * 97), earth().layers(), detail));
        }
        const auto started = std::chrono::steady_clock::now();
        const SurfaceMesh mesh = mesh_of(world, chunks);
        const double seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        WARN("detail " << detail << ": " << 1'000.0 * seconds / static_cast<double>(chunks.size())
                       << " ms a chunk, " << mesh.vertices.size() / chunks.size() << " vertices");
    }
}

namespace {

// Pairs of chunks round `eye` drawn more than a detail apart, or apart at all across a cube's
// edge, as the mesher can't seam.
std::size_t too_far_apart(const Generator& generator, hog::globe::Eye eye, double reach,
                          double ratio) {
    const std::vector<Cell> chunks = hog::globe::chunks_around(generator, eye, reach, ratio);
    const hog::globe::Drawn drawn{chunks};
    const hog::globe::Layers& layers = generator.layers();
    std::size_t too_far = 0;
    for (const Cell chunk : chunks) {
        const hog::globe::CellNeighbors near = hog::globe::around(chunk);
        const hog::globe::CellNeighbors side = hog::globe::sides(chunk);
        for (const Cell beside : near.all()) {
            const std::optional<Cell> over = drawn.over(beside);
            if (!over) {
                continue;
            }
            const bool across_edge = beside.face != chunk.face &&
                                     std::ranges::find(side.all(), beside) != side.all().end();
            const int most_apart = across_edge ? 0 : 1;
            if (over->level + most_apart < chunk.level) {
                ++too_far;
                UNSCOPED_INFO("chunk level "
                              << int{chunk.level} << " face " << int{chunk.face} << " i " << chunk.i
                              << " j " << chunk.j << " beside level " << int{over->level}
                              << " face " << int{over->face} << " i " << over->i << " j " << over->j
                              << " detail " << hog::globe::detail_of(*over, layers) << " eye "
                              << eye.up.x << " " << eye.up.y << " " << eye.up.z << " at "
                              << eye.height);
            }
        }
    }
    return too_far;
}

} // namespace

TEST_CASE("chunks round the client's start keep their neighbours a detail apart at most",
          "[globe][surface]") {
    hog::globe::Planet planet;
    planet.seed = 1;
    const Generator generator{planet};
    const double radius = generator.planet().radius;
    const double height = 30.0;
    const double reach = std::max(1'000.0, std::sqrt((2.0 * radius * height) + (height * height)));
    CHECK(too_far_apart(generator, {.up = DVec3{1.0, 0.0, 0.0}, .height = height}, reach, 3.0) ==
          0);
}

namespace {

// How the chunks beside one are drawn, as the client keys its meshing on: at its detail, coarser,
// finer, or not at all, two bits each.
std::uint32_t drawn_beside(const hog::globe::Drawn& drawn, Cell chunk) {
    std::uint32_t found = 0;
    const hog::globe::CellNeighbors near = hog::globe::around(chunk);
    for (const Cell beside : near.all()) {
        std::uint32_t how = 3;
        if (const std::optional<Cell> over = drawn.over(beside)) {
            how = *over == beside ? 0 : 1;
        } else if (std::ranges::any_of(hog::globe::children(beside),
                                       [&](Cell finer) { return drawn.contains(finer); })) {
            how = 2;
        }
        found = (found << 2U) | how;
    }
    return found;
}

} // namespace

// Flies from the start as the client does, the chunks sought anew every 16 m, checking each set:
// neighbours a detail apart at most, and every chunk's boxes tiling with its neighbours'.
TEST_CASE("chunks keep their neighbours a detail apart at most along a flight",
          "[.gate][globe][surface]") {
    hog::globe::Planet planet;
    planet.seed = 1;
    const Generator generator{planet};
    VoxelStore store{generator.layers()};
    const World world{generator, store};
    const double radius = generator.planet().radius;
    std::size_t overlapping = 0;
    std::set<std::pair<Cell, std::uint32_t>> meshed;
    for (const DVec3 heading :
         {DVec3{0.0, 1.0, 0.0}, DVec3{0.0, 0.0, 1.0}, DVec3{0.0, -0.7, 0.7}}) {
        for (int step = 0; step < 150; ++step) {
            const double height = 30.0 + (step * 2.0);
            const DVec3 up =
                glm::normalize(DVec3{1.0, 0.0, 0.0} + (heading * (16.0 * step / radius)));
            const double reach =
                std::max(1'000.0, std::sqrt((2.0 * radius * height) + (height * height)));
            const hog::globe::Eye eye{.up = up, .height = height};
            CHECK(too_far_apart(generator, eye, reach, 3.0) == 0);
            const std::vector<Cell> chunks = hog::globe::chunks_around(generator, eye, reach, 3.0);
            const hog::globe::Drawn drawn{chunks};
            for (const Cell chunk : chunks) {
                // Only as the client would mesh it anew: with its neighbours drawn differently.
                if (!meshed.emplace(chunk, drawn_beside(drawn, chunk)).second) {
                    continue;
                }
                if (!hog::globe::boxes_tile(world, chunk, drawn)) {
                    ++overlapping;
                    UNSCOPED_INFO("overlap at chunk level "
                                  << int{chunk.level} << " face " << int{chunk.face} << " i "
                                  << chunk.i << " j " << chunk.j << " eye " << up.x << " " << up.y
                                  << " " << up.z << " at " << height);
                }
            }
        }
    }
    CHECK(overlapping == 0);
}

// Flies about from the start as a player might, turning and climbing at random, the chunks sought
// anew every 16 m as the client does, and checks every chunk the client would mesh anew: each
// time one's neighbours are drawn differently, its boxes must tile with theirs.
TEST_CASE("chunks' boxes tile along a wandering flight from the start", "[.gate][globe][surface]") {
    hog::globe::Planet planet;
    planet.seed = 1;
    const Generator generator{planet};
    VoxelStore store{generator.layers()};
    const World world{generator, store};
    const double radius = generator.planet().radius;
    std::mt19937_64 random{7};
    std::uniform_real_distribution<double> unit{-1.0, 1.0};
    DVec3 up{1.0, 0.0, 0.0};
    DVec3 heading{0.0, 1.0, 0.0};
    double height = 30.0;
    std::set<std::pair<Cell, std::uint32_t>> meshed;
    std::size_t overlapping = 0;
    std::size_t too_far = 0;
    for (int step = 0; step < 2'000; ++step) {
        // Turns a little, climbs or dives a little, and flies on 16 to 48 m.
        heading = glm::normalize(heading + (DVec3{unit(random), unit(random), unit(random)} * 0.3));
        heading = glm::normalize(heading - (up * glm::dot(heading, up)));
        height = std::clamp(height * (1.0 + (0.1 * unit(random))), 5.0, 3'000.0);
        up = glm::normalize(up + (heading * ((32.0 + (16.0 * unit(random))) / radius)));
        const double reach =
            std::max(1'000.0, std::sqrt((2.0 * radius * height) + (height * height)));
        const hog::globe::Eye eye{.up = up, .height = height};
        too_far += too_far_apart(generator, eye, reach, 3.0);
        const std::vector<Cell> chunks = hog::globe::chunks_around(generator, eye, reach, 3.0);
        const hog::globe::Drawn drawn{chunks};
        for (const Cell chunk : chunks) {
            if (!meshed.emplace(chunk, drawn_beside(drawn, chunk)).second) {
                continue;
            }
            if (!hog::globe::boxes_tile(world, chunk, drawn)) {
                ++overlapping;
                UNSCOPED_INFO("overlap at chunk level "
                              << int{chunk.level} << " face " << int{chunk.face} << " i " << chunk.i
                              << " j " << chunk.j << " step " << step << " eye " << up.x << " "
                              << up.y << " " << up.z << " at " << height);
            }
        }
    }
    CAPTURE(meshed.size());
    CHECK(too_far == 0);
    CHECK(overlapping == 0);
}

// Where the client aborted, 250 km up with the whole planet in view: a level 2 chunk on a cube's
// edge found its boxes overlapping a neighbour's.
TEST_CASE("chunks' boxes tile from far out over a cube's edge", "[globe][surface]") {
    hog::globe::Planet planet;
    planet.seed = 1;
    const Generator generator{planet};
    VoxelStore store{generator.layers()};
    const World world{generator, store};
    const hog::globe::Eye eye{.up = DVec3{-0.417842442, -0.555391738, 0.718990758},
                              .height = 251'709.3};
    const std::vector<Cell> chunks = hog::globe::chunks_around(generator, eye, 348'330.0, 3.0);
    const hog::globe::Drawn drawn{chunks};
    CHECK(too_far_apart(generator, eye, 348'330.0, 3.0) == 0);
    for (const Cell chunk : chunks) {
        CAPTURE(int{chunk.level}, int{chunk.face}, chunk.i, chunk.j);
        CHECK(hog::globe::boxes_tile(world, chunk, drawn));
    }
}

namespace {

// The share of a mesh's vertices on the ground within `within` that wear each material; those
// down caves don't count, as coarser detail doesn't carve them.
std::map<Material, double> material_shares(const SurfaceMesh& mesh, Cell within) {
    std::map<Material, double> shares;
    double counted = 0.0;
    for (const auto& vertex : mesh.vertices) {
        const DVec3 direction = glm::normalize(mesh.origin + DVec3{vertex.position});
        if (vertex.material == Material::Water ||
            hog::globe::cell_under(direction, within.level) != within) {
            continue;
        }
        const double ground = hog::globe::ground_below(earth(), direction, 1.0).value_or(0.0);
        if (glm::length(mesh.origin + DVec3{vertex.position}) < ground - 4.0) {
            continue;
        }
        shares[vertex.material] += 1.0;
        counted += 1.0;
    }
    for (auto& [material, share] : shares) {
        share /= std::max(counted, 1.0);
    }
    return shares;
}

} // namespace

// The ground's cover is thinner than a coarse voxel, so a coarse chunk's voxels would show the
// rock under it where fine ones show the cover; each must show what's at its top instead.
TEST_CASE("the ground wears the same materials however coarse it's drawn", "[globe][surface]") {
    VoxelStore store{earth().layers()};
    const World world{earth(), store};
    const hog::globe::Layers& layers = earth().layers();
    for (const std::uint64_t k : {3ULL, 11ULL, 29ULL}) {
        const Cell fine = hog::globe::chunk_under(somewhere(k), layers, 0);
        const Cell coarse = hog::globe::chunk_under(somewhere(k), layers, 3);
        const std::map<Material, double> fine_shares =
            material_shares(mesh_of(world, std::array{fine}), fine);
        const std::map<Material, double> coarse_shares =
            material_shares(mesh_of(world, std::array{coarse}), fine);
        REQUIRE_FALSE(fine_shares.empty());
        REQUIRE_FALSE(coarse_shares.empty());
        const auto most = std::ranges::max_element(fine_shares, {},
                                                   [](const auto& share) { return share.second; });
        std::string listed;
        for (const auto& [material, share] : coarse_shares) {
            listed += std::format("{}:{:.2f} ", static_cast<int>(material), share);
        }
        CAPTURE(k, static_cast<int>(most->first), most->second, listed);
        const auto coarse_most = coarse_shares.find(most->first);
        const double coarse_share = coarse_most == coarse_shares.end() ? 0.0 : coarse_most->second;
        CAPTURE(coarse_share);
        CHECK(coarse_share > most->second - 0.3);
    }
}
