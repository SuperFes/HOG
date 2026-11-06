#include <hog/chunks/mesh_codec.hpp>

#include <algorithm>
#include <format>
#include <unordered_map>
#include <utility>

namespace hog::chunks {

Result<protocol::SentMesh> to_sent(const terrain::BlockMesh& mesh, Mesh which) {
    const bool ground = which == Mesh::Ground;
    if (mesh.vertices.size() >
            (ground ? protocol::most_ground_vertices : protocol::most_water_vertices) ||
        mesh.indices.size() >
            (ground ? protocol::most_ground_indices : protocol::most_water_indices)) {
        return make_error(ErrorCode::OutOfRange,
                          std::format("a mesh of {} vertices and {} indices is too big to send",
                                      mesh.vertices.size(), mesh.indices.size()));
    }
    protocol::SentMesh sent{
        .origin = protocol::to_millimetres(mesh.origin), .vertices = {}, .indices = mesh.indices};
    sent.vertices.reserve(mesh.vertices.size());
    for (const terrain::BlockVertex& vertex : mesh.vertices) {
        sent.vertices.push_back(
            {.position = {vertex.position.x, vertex.position.y, vertex.position.z},
             .normal = {vertex.normal.x, vertex.normal.y, vertex.normal.z},
             .tiles = vertex.tiles,
             .corner = vertex.corner});
    }
    return sent;
}

terrain::BlockMesh from_sent(const protocol::SentMesh& mesh) {
    terrain::BlockMesh shown{
        .origin = protocol::to_world(mesh.origin), .vertices = {}, .indices = mesh.indices};
    shown.vertices.reserve(mesh.vertices.size());
    for (const protocol::MeshVertex& vertex : mesh.vertices) {
        shown.vertices.push_back(
            {.position = {vertex.position[0], vertex.position[1], vertex.position[2]},
             .normal = {vertex.normal[0], vertex.normal[1], vertex.normal[2]},
             .tiles = vertex.tiles,
             .corner = vertex.corner});
    }
    return shown;
}

namespace {

// A mesh cut into pieces by its triangles, each within the most vertices and indices a part has.
std::vector<terrain::BlockMesh> cut(const terrain::BlockMesh& mesh, std::size_t most_vertices,
                                    std::size_t most_indices) {
    std::vector<terrain::BlockMesh> pieces;
    const auto fresh = [&] {
        pieces.push_back({.origin = mesh.origin, .vertices = {}, .indices = {}});
        return std::unordered_map<std::uint32_t, std::uint32_t>{};
    };
    std::unordered_map<std::uint32_t, std::uint32_t> placed = fresh();
    for (std::size_t corner = 0; corner + 2 < mesh.indices.size(); corner += 3) {
        terrain::BlockMesh& piece = pieces.back();
        std::size_t new_vertices = 0;
        for (std::size_t at = 0; at < 3; ++at) {
            new_vertices += placed.contains(mesh.indices[corner + at]) ? 0 : 1;
        }
        if (!piece.indices.empty() && (piece.vertices.size() + new_vertices > most_vertices ||
                                       piece.indices.size() + 3 > most_indices)) {
            placed = fresh();
        }
        terrain::BlockMesh& into = pieces.back();
        for (std::size_t at = 0; at < 3; ++at) {
            const std::uint32_t from = mesh.indices[corner + at];
            const auto found = placed.find(from);
            if (found == placed.end()) {
                placed.emplace(from, static_cast<std::uint32_t>(into.vertices.size()));
                into.indices.push_back(static_cast<std::uint32_t>(into.vertices.size()));
                into.vertices.push_back(mesh.vertices[from]);
            } else {
                into.indices.push_back(found->second);
            }
        }
    }
    return pieces;
}

} // namespace

Result<std::vector<protocol::Chunk>> to_parts(std::uint64_t key, const terrain::BlockMesh& ground,
                                              const terrain::BlockMesh& water) {
    auto sent_water = to_sent(water, Mesh::Water);
    if (!sent_water) {
        return std::unexpected(sent_water.error());
    }
    std::vector<protocol::Chunk> parts;
    if (const auto whole = to_sent(ground, Mesh::Ground)) {
        parts.push_back(
            {.key = key, .part = 0, .parts = 1, .ground = *whole, .water = *sent_water});
        return parts;
    }
    const std::vector<terrain::BlockMesh> pieces =
        cut(ground, protocol::most_ground_vertices, protocol::most_ground_indices);
    if (pieces.size() > 255) {
        return make_error(ErrorCode::OutOfRange,
                          std::format("a chunk's ground needs {} parts", pieces.size()));
    }
    for (std::size_t at = 0; at < pieces.size(); ++at) {
        auto sent = to_sent(pieces[at], Mesh::Ground);
        if (!sent) {
            return std::unexpected(sent.error());
        }
        parts.push_back({.key = key,
                         .part = static_cast<std::uint8_t>(at),
                         .parts = static_cast<std::uint8_t>(pieces.size()),
                         .ground = std::move(*sent),
                         .water = at == 0 ? *sent_water : protocol::SentMesh{}});
    }
    return parts;
}

std::optional<protocol::Chunk> ChunkParts::add(protocol::Chunk part) {
    if (part.parts == 1) {
        waiting_.erase(part.key);
        return part;
    }
    std::vector<std::optional<protocol::Chunk>>& held = waiting_[part.key];
    // Parts of another cut of the chunk start over.
    if (held.size() != part.parts) {
        held.assign(part.parts, std::nullopt);
    }
    const std::uint8_t at = part.part;
    held[at] = std::move(part);
    if (std::ranges::any_of(held, [](const auto& one) { return !one.has_value(); })) {
        return std::nullopt;
    }
    protocol::Chunk whole{.key = held[0]->key, .part = 0, .parts = 1, .ground = {}, .water = {}};
    whole.ground.origin = held[0]->ground.origin;
    whole.water = held[0]->water;
    for (const auto& one : held) {
        const auto offset = static_cast<std::uint32_t>(whole.ground.vertices.size());
        whole.ground.vertices.insert(whole.ground.vertices.end(), one->ground.vertices.begin(),
                                     one->ground.vertices.end());
        for (const std::uint32_t index : one->ground.indices) {
            whole.ground.indices.push_back(index + offset);
        }
    }
    waiting_.erase(whole.key);
    return whole;
}

} // namespace hog::chunks
