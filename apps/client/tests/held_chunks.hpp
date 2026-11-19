#pragma once

#include <hog/client/held_pieces.hpp>
#include <hog/core/result.hpp>
#include <hog/grid/piece.hpp>
#include <hog/protocol/messages.hpp>

#include <cstdint>
#include <optional>
#include <vector>

namespace hog::testing {

// Each tree `piece` holds as the chunk the server would send, from band `band`: by default as
// deep as the tree reaches, so one reaching deeper stays over one at its root reaching less.
inline std::vector<protocol::SentChunk> chunks_of(const grid::Piece& piece,
                                                  std::optional<std::uint8_t> band = std::nullopt,
                                                  std::uint64_t version = 1) {
    std::vector<protocol::SentChunk> chunks;
    const auto add = [&](std::uint8_t kind, const auto& tree) {
        if (!tree) {
            return;
        }
        chunks.push_back(
            {.kind = kind,
             .band = band.value_or(static_cast<std::uint8_t>(piece.root.depth + tree->levels())),
             .depth = piece.root.depth,
             .x = piece.root.x,
             .y = piece.root.y,
             .z = piece.root.z,
             .version = version,
             .tree = tree->encode()});
    };
    add(0, piece.shape);
    add(1, piece.material);
    add(2, piece.water);
    add(3, piece.woods);
    return chunks;
}

// Has `held` take each tree of `piece` as its chunk, as chunks_of gives them.
inline Result<void> hold(client::HeldPieces& held, const grid::Piece& piece,
                         std::optional<std::uint8_t> band = std::nullopt,
                         std::uint64_t version = 1) {
    for (const protocol::SentChunk& chunk : chunks_of(piece, band, version)) {
        if (auto taken = held.take(chunk); !taken) {
            return taken;
        }
    }
    return {};
}

} // namespace hog::testing
