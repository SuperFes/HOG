#pragma once

#include <hog/protocol/messages.hpp>

#include <cstddef>
#include <utility>
#include <vector>

namespace hog::server {

// Sends chunks, a chunk's parts together or not at all, while the connection has less than `limit`
// bytes queued when a chunk is begun: one may go over it, however big, else a chunk bigger than the
// limit would never go, and parts split between ticks would be sent over and over. `send` takes a
// message's bytes and says whether it was queued. A chunk begun once is sent whole, and the chunks
// after one that waits wait too, so the nearest go first. Returns those that were not sent, to be
// asked for again.
template <typename Send>
[[nodiscard]] std::vector<protocol::Chunk> send_chunks(std::vector<protocol::Chunk> chunks,
                                                       std::size_t queued, std::size_t limit,
                                                       Send&& send) {
    std::vector<protocol::Chunk> unsent;
    bool waiting = false; // a chunk waited, so those after it do
    bool begun = false;   // the chunk in hand is being sent
    for (protocol::Chunk& part : chunks) {
        if (part.part == 0) {
            begun = !waiting && queued < limit;
            waiting = waiting || !begun;
        }
        if (!begun) {
            unsent.push_back(std::move(part));
            continue;
        }
        const std::vector<std::byte> bytes = protocol::encode(part);
        if (!send(bytes)) {
            begun = false;
            waiting = true;
            unsent.push_back(std::move(part));
            continue;
        }
        queued += bytes.size();
    }
    return unsent;
}

} // namespace hog::server
