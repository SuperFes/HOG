#include <hog/chunks/mesh_codec.hpp>
#include <hog/core/log.hpp>
#include <hog/server/chunk_sharing.hpp>

#include <algorithm>
#include <limits>
#include <unordered_map>
#include <utility>

namespace hog::server {

namespace {

const log::Category chunks_log{"chunks"};

} // namespace

ChunkSharing::ChunkSharing(const globe::World& world, jobs::JobSystem& jobs,
                           chunks::GlobeDetail detail)
    : source_{&world}, jobs_{&jobs}, detail_{detail}, copy_{world.generator().layers()},
      seen_{world.generator(), copy_} {
    // The world as the server holds it now is what's shown at first.
    for (const globe::BrickSite site : world.store().sites()) {
        copy_.put(site, *world.store().brick(site));
    }
    synced_ = world.store().revision();
}

void ChunkSharing::join(net::ConnectionId player) {
    players_.insert_or_assign(
        player, std::make_unique<Player>(chunks::GlobeStream{seen_, *jobs_, detail_}));
}

void ChunkSharing::leave(net::ConnectionId player) {
    if (const auto found = players_.find(player); found != players_.end()) {
        found->second->stream.settle();
        players_.erase(found);
    }
}

void ChunkSharing::detail(net::ConnectionId player, const protocol::ViewDetail& asked) {
    if (const auto found = players_.find(player); found != players_.end()) {
        found->second->stream.set_detail({.ratio = static_cast<double>(asked.ratio),
                                          .least = static_cast<double>(asked.near),
                                          .slope = static_cast<double>(asked.slope)});
    }
}

void ChunkSharing::follow(net::ConnectionId player, DVec3 eye, DVec3 forward) {
    if (const auto found = players_.find(player); found != players_.end()) {
        found->second->stream.follow(eye, forward);
    }
}

void ChunkSharing::step() {
    // Only when the world has changed, and not oftener than sync_every: each sync waits for the
    // chunks being meshed to finish, which stalls meshing; an edit is carried over at once.
    if (!syncing_) {
        if (++ticks_ >= sync_every && source_->store().revision() != synced_) {
            syncing_ = true;
        }
        if (!syncing_) {
            return;
        }
    }
    // Holds new chunks back until those meshing are done.
    bool meshing = false;
    for (auto& [id, player] : players_) {
        player->stream.hold(true);
        meshing = meshing || player->stream.meshing() > 0;
    }
    if (meshing) {
        return;
    }
    const globe::VoxelStore& store = source_->store();
    for (const globe::BrickSite site : store.changed_since(synced_)) {
        if (const globe::Brick* brick = store.brick(site)) {
            copy_.put(site, *brick);
        } else {
            copy_.erase(site);
        }
        for (auto& [id, player] : players_) {
            player->stream.changed(site);
        }
    }
    synced_ = store.revision();
    for (auto& [id, player] : players_) {
        player->stream.hold(false);
    }
    syncing_ = false;
    ticks_ = 0;
}

void ChunkSharing::edited() {
    ticks_ = sync_every;
}

void ChunkSharing::ask(net::ConnectionId id, std::span<const std::uint64_t> keys) {
    if (const auto found = players_.find(id); found != players_.end()) {
        found->second->asked.insert(keys.begin(), keys.end());
    }
}

ChunkSharing::Next ChunkSharing::next(net::ConnectionId id, std::size_t most) {
    Next out;
    const auto found = players_.find(id);
    if (found == players_.end()) {
        return out;
    }
    Player& player = *found->second;
    chunks::GlobeStream::Changes changes = player.stream.collect();
    for (const chunks::GlobeStream::Shown& shown : changes.shown) {
        const std::uint32_t version = ++player.versions[shown.key];
        auto parts = chunks::to_parts(shown.key, *shown.ground, *shown.water);
        if (!parts) {
            // Shown empty, rather than not at all, so nothing waits for it.
            log::warn(chunks_log, "chunk {:#x}: {}", shown.key, parts.error());
            parts = std::vector<protocol::Chunk>{
                {.key = shown.key, .version = 0, .part = 0, .parts = 1, .ground = {}, .water = {}}};
        }
        for (protocol::Chunk& part : *parts) {
            part.version = version;
        }
        player.meshed.insert_or_assign(shown.key,
                                       Meshed{.version = version, .parts = std::move(*parts)});
    }
    for (const std::uint64_t key : changes.gone) {
        player.meshed.erase(key);
        player.asked.erase(key);
    }

    // Each chunk's place in the order wanted, nearest first, and the plan it makes.
    std::unordered_map<std::uint64_t, std::size_t> rank;
    std::vector<protocol::PlannedChunk> plan;
    const std::span<const globe::Cell> wanted = player.stream.wanted();
    for (std::size_t at = 0; at < wanted.size(); ++at) {
        const std::uint64_t key = chunks::globe_key(wanted[at]);
        rank.emplace(key, at);
        if (plan.size() < protocol::most_planned) {
            const auto meshed = player.meshed.find(key);
            plan.push_back({.key = key,
                            .version = meshed != player.meshed.end() ? meshed->second.version : 0});
        }
    }
    // Sent when it changes, not oftener than every plan_every, and now and then though it hasn't.
    const auto now = std::chrono::steady_clock::now();
    const bool changed = plan != player.planned;
    if (!plan.empty() && ((changed && now - player.planned_at >= plan_every) ||
                          now - player.planned_at >= plan_again)) {
        out.plan = protocol::ChunkPlan{.wanted = plan};
        player.planned = std::move(plan);
        player.planned_at = now;
    }

    // Now and then, how the player's chunks are coming.
    if (now - player.reported >= std::chrono::seconds{5}) {
        player.reported = now;
        log::debug(chunks_log, "{} wanted, {} meshed, {} asked, {} meshing, {} in flight",
                   wanted.size(), player.meshed.size(), player.asked.size(),
                   player.stream.meshing(), player.stream.in_flight());
    }

    // Those asked for that are meshed, nearest first.
    std::vector<std::uint64_t> ready;
    for (const std::uint64_t key : player.asked) {
        if (player.meshed.contains(key)) {
            ready.push_back(key);
        }
    }
    const auto rank_of = [&](std::uint64_t key) {
        const auto place = rank.find(key);
        return place != rank.end() ? place->second : std::numeric_limits<std::size_t>::max();
    };
    std::ranges::sort(ready,
                      [&](std::uint64_t a, std::uint64_t b) { return rank_of(a) < rank_of(b); });
    for (const std::uint64_t key : ready) {
        if (out.chunks.size() >= most) {
            break;
        }
        for (const protocol::Chunk& part : player.meshed.at(key).parts) {
            out.chunks.push_back(part);
        }
        player.asked.erase(key);
    }
    return out;
}

void ChunkSharing::retry_plan(net::ConnectionId id) {
    if (const auto found = players_.find(id); found != players_.end()) {
        found->second->planned.clear();
        found->second->planned_at = {};
    }
}

void ChunkSharing::retry(net::ConnectionId id, protocol::Chunk chunk) {
    if (const auto found = players_.find(id); found != players_.end()) {
        found->second->asked.insert(chunk.key);
    }
}

std::span<const globe::Cell> ChunkSharing::wanted(net::ConnectionId player) const {
    const auto found = players_.find(player);
    return found == players_.end() ? std::span<const globe::Cell>{}
                                   : found->second->stream.wanted();
}

std::size_t ChunkSharing::waiting(net::ConnectionId player) const {
    const auto found = players_.find(player);
    if (found == players_.end()) {
        return 0;
    }
    return static_cast<std::size_t>(
        std::ranges::count_if(found->second->asked, [&](std::uint64_t key) {
            return found->second->meshed.contains(key);
        }));
}

} // namespace hog::server
