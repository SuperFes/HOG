#include <hog/chunks/mesh_codec.hpp>
#include <hog/protocol/messages.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <vector>

using namespace hog;

namespace {

terrain::BlockMesh triangle() {
    return {.origin = WorldPosition{DVec3{1.0, 2.0, 3.0}},
            .vertices = {{.position = {0.0F, 1.0F, 2.0F},
                          .normal = {0.0F, 1.0F, 0.0F},
                          .tiles = {3, 3, 3},
                          .corner = terrain::packed_corner(0, 0.5)},
                         {.position = {4.0F, 5.0F, 6.0F},
                          .normal = {1.0F, 0.0F, 0.0F},
                          .tiles = {3, 4, 5},
                          .corner = terrain::packed_corner(1, 0.0)},
                         {.position = {-1.0F, 0.5F, 0.25F},
                          .normal = {0.0F, 0.0F, 1.0F},
                          .tiles = {3, 4, 5},
                          .corner = terrain::packed_corner(2, 0.0, 0.5)}},
            .indices = {0, 1, 2}};
}

} // namespace

TEST_CASE("a mesh sent is the mesh shown, to the millimetre of its origin", "[chunks][codec]") {
    const terrain::BlockMesh mesh = triangle();
    const auto sent = chunks::to_sent(mesh, chunks::Mesh::Ground);
    REQUIRE(sent);
    // Through the wire too.
    const auto read = protocol::decode(protocol::encode(
        protocol::Chunk{.key = 5, .part = 0, .parts = 1, .ground = *sent, .water = {}}));
    REQUIRE(read);
    const auto& chunk = std::get<protocol::Chunk>(*read);
    CHECK(chunk.key == 5);
    const terrain::BlockMesh shown = chunks::from_sent(chunk.ground);
    CHECK(shown.indices == mesh.indices);
    REQUIRE(shown.vertices.size() == mesh.vertices.size());
    for (std::size_t at = 0; at < mesh.vertices.size(); ++at) {
        CHECK(shown.vertices[at] == mesh.vertices[at]);
    }
    CHECK(glm::length(shown.origin.metres() - mesh.origin.metres()) < 1e-3);
}

TEST_CASE("a mesh too big for a message isn't sent", "[chunks][codec]") {
    terrain::BlockMesh mesh = triangle();
    mesh.indices.assign(protocol::most_ground_indices + 3, 0);
    CHECK_FALSE(chunks::to_sent(mesh, chunks::Mesh::Ground));
    mesh = triangle();
    mesh.vertices.resize(protocol::most_ground_vertices + 1);
    CHECK_FALSE(chunks::to_sent(mesh, chunks::Mesh::Ground));
    mesh = triangle();
    mesh.vertices.resize(protocol::most_water_vertices + 1);
    CHECK_FALSE(chunks::to_sent(mesh, chunks::Mesh::Water));
    CHECK(chunks::to_sent(mesh, chunks::Mesh::Ground));
}

namespace {

// A strip of `triangles` triangles, each with a vertex of its own so none is shared,
// distinguishable by position.
terrain::BlockMesh strip(std::size_t triangles) {
    terrain::BlockMesh mesh{
        .origin = WorldPosition{DVec3{5.0, 6.0, 7.0}}, .vertices = {}, .indices = {}};
    for (std::size_t at = 0; at < triangles; ++at) {
        for (std::size_t corner = 0; corner < 3; ++corner) {
            mesh.indices.push_back(static_cast<std::uint32_t>(mesh.vertices.size()));
            mesh.vertices.push_back(
                {.position = {static_cast<float>(at), static_cast<float>(corner), 0.0F},
                 .normal = {0.0F, 0.0F, 1.0F},
                 .tiles = {1, 1, 1},
                 .corner = static_cast<std::uint8_t>(corner)});
        }
    }
    return mesh;
}

// The triangles of a mesh as the positions of their corners, in order.
std::vector<float> corners_of(const terrain::BlockMesh& mesh) {
    std::vector<float> found;
    for (const std::uint32_t index : mesh.indices) {
        found.push_back(mesh.vertices[index].position.x);
        found.push_back(mesh.vertices[index].position.y);
    }
    return found;
}

} // namespace

TEST_CASE("a chunk that fits a message is one part, and a bigger one is cut and put back together",
          "[chunks][codec]") {
    const auto small = chunks::to_parts(9, strip(100), {});
    REQUIRE(small);
    CHECK(small->size() == 1);
    CHECK(small->front().parts == 1);

    // More vertices than a message of ground carries.
    const terrain::BlockMesh big = strip((protocol::most_ground_vertices / 3) * 2 + 7);
    const auto parts = chunks::to_parts(9, big, strip(4));
    REQUIRE(parts);
    REQUIRE(parts->size() > 1);
    chunks::ChunkParts assembly;
    std::optional<protocol::Chunk> whole;
    // The last part first: in any order.
    for (std::size_t at = parts->size(); at-- > 0;) {
        const protocol::Chunk& part = (*parts)[at];
        CHECK(part.ground.vertices.size() <= protocol::most_ground_vertices);
        CHECK(part.water.vertices.empty() == (at != 0));
        CHECK_FALSE(whole);
        whole = assembly.add(part);
    }
    REQUIRE(whole);
    CHECK(assembly.waiting() == 0);
    const terrain::BlockMesh shown = chunks::from_sent(whole->ground);
    CHECK(corners_of(shown).size() == corners_of(big).size());
    // Every triangle comes back, though not in the order they were cut.
    auto sorted = [](std::vector<float> values) {
        std::vector<std::array<float, 2>> pairs;
        for (std::size_t at = 0; at + 1 < values.size(); at += 2) {
            pairs.push_back({values[at], values[at + 1]});
        }
        std::ranges::sort(pairs);
        return pairs;
    };
    CHECK(sorted(corners_of(shown)) == sorted(corners_of(big)));
    CHECK(whole->water.vertices.size() == 12);
}

TEST_CASE("parts of a chunk sent again start over, and a whole chunk replaces waiting parts",
          "[chunks][codec]") {
    const terrain::BlockMesh big = strip((protocol::most_ground_vertices / 3) * 2 + 7);
    const auto parts = chunks::to_parts(9, big, {});
    REQUIRE(parts);
    chunks::ChunkParts assembly;
    CHECK_FALSE(assembly.add((*parts)[0]));
    CHECK(assembly.waiting() == 1);
    // A whole one of the key comes, and nothing waits for the rest.
    protocol::Chunk whole{.key = 9, .part = 0, .parts = 1, .ground = {}, .water = {}};
    CHECK(assembly.add(whole));
    CHECK(assembly.waiting() == 0);
    // Dropped parts are forgotten.
    CHECK_FALSE(assembly.add((*parts)[0]));
    assembly.drop(9);
    CHECK(assembly.waiting() == 0);
}
