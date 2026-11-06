#include <hog/globe/brick_archive.hpp>
#include <hog/net/events.hpp>
#include <hog/server/brick_sharing.hpp>

#include <cstddef>
#include <utility>

namespace hog::server {

namespace {

// Bytes of bricks a message carries at most, leaving the other half for their keys and lengths
// and well under what may be sent.
constexpr std::size_t most_batch_bytes = net::max_message_size / 2;

} // namespace

std::vector<protocol::Bricks> BrickSharing::all() const {
    return batched(store_->sites());
}

std::vector<protocol::Bricks> BrickSharing::changed() {
    std::vector<protocol::Bricks> messages = batched(store_->changed_since(told_));
    told_ = store_->revision();
    return messages;
}

std::vector<protocol::Bricks> batch_bricks(std::vector<protocol::SentBrick> bricks) {
    std::vector<protocol::Bricks> messages;
    protocol::Bricks batch;
    std::size_t bytes = 0;
    for (protocol::SentBrick& brick : bricks) {
        if (!batch.bricks.empty() && (batch.bricks.size() == protocol::most_bricks ||
                                      bytes + brick.bytes.size() > most_batch_bytes)) {
            messages.push_back(std::exchange(batch, {}));
            bytes = 0;
        }
        bytes += brick.bytes.size();
        batch.bricks.push_back(std::move(brick));
    }
    if (!batch.bricks.empty()) {
        messages.push_back(std::move(batch));
    }
    return messages;
}

std::vector<protocol::Bricks> BrickSharing::batched(std::span<const globe::BrickSite> sites) const {
    std::vector<protocol::SentBrick> bricks;
    bricks.reserve(sites.size());
    for (const globe::BrickSite site : sites) {
        globe::SavedBrick coded = globe::coded_brick(*store_, site);
        bricks.push_back({.key = coded.key, .bytes = std::move(coded.bytes)});
    }
    return batch_bricks(std::move(bricks));
}

} // namespace hog::server
