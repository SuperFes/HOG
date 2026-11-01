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
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
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

// A direction whose ground stands between two heights over the radius.
DVec3 ground_between(const Generator& generator, double low, double high) {
    for (std::uint64_t k = 0;; ++k) {
        const DVec3 direction = somewhere(k);
        const double ground = hog::globe::ground_below(generator, direction, 0.5).value_or(0.0) -
                              generator.planet().radius;
        if (ground > low && ground < high) {
            return direction;
        }
    }
}

SurfaceMesh mesh_of(const World& world, std::span<const Cell> chunks) {
    SurfaceMesh mesh;
    mesh.origin = hog::globe::center(chunks.front()) * world.generator().planet().radius;
    for (const Cell chunk : chunks) {
        hog::globe::add_chunk(mesh, world, chunk);
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

TEST_CASE("chunks at other details meet without gaps, on a face and across the cube's edges",
          "[globe][surface]") {
    VoxelStore store{venus().layers()};
    const World world{venus(), store};
    const hog::globe::Layers& layers = venus().layers();
    for (const DVec3 up :
         {somewhere(11), glm::normalize(DVec3{1.0, 1.0, 0.2}),
          glm::normalize(DVec3{-0.3, 1.0, -1.0}), glm::normalize(DVec3{-1.0, -1.0, 0.3})}) {
        // Close round the eye, so the details change within a few chunks of it.
        const std::vector<Cell> chunks = hog::globe::chunks_around(venus(), {.up = up}, 100.0, 1.1);
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
        REQUIRE(details.size() >= 3);
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
                                                << int{chunk->face} << " "
                                                << glm::length(side) - ground << " m over");
            }
        }
        CHECK(open == 0);
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

TEST_CASE("only the cube's corners leave a gap, a column wide", "[globe][surface]") {
    VoxelStore store{moon().layers()};
    const World world{moon(), store};
    const hog::globe::Layers& layers = moon().layers();
    const DVec3 corner = glm::normalize(DVec3{1.0, 1.0, 1.0});
    const Cell chunk = hog::globe::chunk_under(corner, layers);
    const SurfaceMesh mesh = mesh_of(world, block_round(chunk));
    REQUIRE(block_round(chunk).size() == 8);
    const double radius = moon().planet().radius;
    for (const DVec3 side : open_sides(mesh)) {
        const DVec3 up = glm::normalize(side);
        if (hog::globe::chunk_under(up, layers) != chunk) {
            continue;
        }
        const double ground = hog::globe::ground_below(moon(), up, 0.5).value_or(0.0);
        if (glm::length(side) < ground - hog::globe::surface_depth + 2.0) {
            continue;
        }
        CHECK(glm::length(up - corner) * radius < 3.0);
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
    CHECK(worst < 0.1);
    CHECK(total / near_lip < 0.04);
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
