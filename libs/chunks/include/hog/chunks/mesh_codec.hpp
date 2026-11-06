#pragma once

#include <hog/core/result.hpp>
#include <hog/protocol/messages.hpp>
#include <hog/terrain/block_mesh.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace hog::chunks {

// Which of a chunk's meshes, for how many it may have.
enum class Mesh : std::uint8_t { Ground, Water };

// A mesh as it's sent. Fails for one with more vertices or indices than a message carries.
[[nodiscard]] Result<protocol::SentMesh> to_sent(const terrain::BlockMesh& mesh, Mesh which);

// A mesh as it was sent, to be shown.
[[nodiscard]] terrain::BlockMesh from_sent(const protocol::SentMesh& mesh);

// A chunk's meshes as the messages that carry them: one, or, where the ground is too big for one,
// several parts of it, the water with the first. Fails where the water is too big, or the ground
// would need more than 255 parts.
[[nodiscard]] Result<std::vector<protocol::Chunk>>
to_parts(std::uint64_t key, const terrain::BlockMesh& ground, const terrain::BlockMesh& water);

// Takes in a chunk's parts, in any order, and gives the whole once all of a key's are in: its
// ground the parts' laid end to end. A whole chunk, or a part of one sent again, replaces any
// parts waiting.
class ChunkParts {
public:
    [[nodiscard]] std::optional<protocol::Chunk> add(protocol::Chunk part);
    // Forgets the parts waiting for a key, or for any.
    void drop(std::uint64_t key) { waiting_.erase(key); }
    void clear() noexcept { waiting_.clear(); }
    // Keys with parts waiting.
    [[nodiscard]] std::size_t waiting() const noexcept { return waiting_.size(); }

private:
    std::map<std::uint64_t, std::vector<std::optional<protocol::Chunk>>> waiting_;
};

} // namespace hog::chunks
