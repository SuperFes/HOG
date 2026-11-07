#pragma once

#include <hog/chunks/globe_stream.hpp>
#include <hog/core/math.hpp>
#include <hog/globe/brick.hpp>
#include <hog/globe/volume.hpp>
#include <hog/globe/world.hpp>
#include <hog/jobs/job_system.hpp>
#include <hog/net/events.hpp>
#include <hog/protocol/messages.hpp>

#include <chrono>
#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace hog::server {

// The surfaces of the planet each player is shown: what's wanted round them, coarser further off,
// nearest first, as a plan sent whenever it changes, and the chunks of it they ask for, sent a few
// at a time once meshed. The server never takes a chunk back; the player keeps what it likes. The
// chunks are meshed from a copy of the world the changes are carried into now and then, when none
// is being meshed, so the world is changed whenever and never waited on; what's shown follows a
// moment behind. The world must outlive it.
class ChunkSharing {
public:
    ChunkSharing(const globe::World& world, jobs::JobSystem& jobs, chunks::GlobeDetail detail = {});

    // A player joined, or left.
    void join(net::ConnectionId player);
    void leave(net::ConnectionId player);

    // Wants the detail to fall off as a player asks, within what's allowed.
    void detail(net::ConnectionId player, const protocol::ViewDetail& asked);

    // Wants the surfaces round a player's eye, in metres from the planet's centre.
    // Looking along `forward`, if known, those in view come first.
    void follow(net::ConnectionId player, DVec3 eye, DVec3 forward = {});

    // Once a tick: carries the world's changes into the copy the chunks are meshed from, every
    // `sync_every` calls, once the chunks being meshed are done.
    void step();

    // The world was edited by a player, who is to see it soon: carried over at the next step.
    void edited();

    // The world's volume was put back whole, as from its file: meshed afresh from now on.
    void reload();

    // The player lacks these chunks of the plan, and wants them when meshed.
    void ask(net::ConnectionId player, std::span<const std::uint64_t> keys);

    // What a player is to be sent now: the plan, if it's changed or is due again, and of the chunks
    // they asked for that are meshed, nearest first, at most `most` messages' worth.
    struct Next {
        std::optional<protocol::ChunkPlan> plan;
        std::vector<protocol::Chunk> chunks;
    };
    [[nodiscard]] Next next(net::ConnectionId player, std::size_t most);

    // A chunk that couldn't be sent, to be sent again; and a plan that couldn't be, to be sent
    // again.
    void retry(net::ConnectionId player, protocol::Chunk chunk);
    void retry_plan(net::ConnectionId player);

    // The chunks wanted round a player, nearest first.
    [[nodiscard]] std::span<const globe::Cell> wanted(net::ConnectionId player) const;

    // Chunks a player asked for that are meshed and not yet sent.
    [[nodiscard]] std::size_t waiting(net::ConnectionId player) const;

private:
    // A chunk as last meshed, as the messages that carry it.
    struct Meshed {
        std::uint32_t version = 0;
        std::vector<protocol::Chunk> parts;
    };

    struct Player {
        explicit Player(chunks::GlobeStream stream) : stream{std::move(stream)} {}
        chunks::GlobeStream stream;
        std::unordered_map<std::uint64_t, Meshed> meshed;
        // How many times each has been meshed, which a chunk meshed again goes up from.
        std::unordered_map<std::uint64_t, std::uint32_t> versions;
        // Keys asked for and not yet sent.
        std::unordered_set<std::uint64_t> asked;
        // The plan as last sent, and when; cleared to have it sent again.
        std::vector<protocol::PlannedChunk> planned;
        std::chrono::steady_clock::time_point planned_at{};
        std::chrono::steady_clock::time_point reported{};
    };

    // The shortest wait between plans, and the longest before one is sent again though unchanged.
    static constexpr std::chrono::milliseconds plan_every{100};
    static constexpr std::chrono::seconds plan_again{2};

    // Calls of step() between carrying the world's changes over, at the soonest.
    static constexpr int sync_every = 100;

    const globe::World* source_;
    jobs::JobSystem* jobs_;
    chunks::GlobeDetail detail_;
    // What the chunks are meshed from: the source's volume as of the last sync.
    globe::Volume copy_;
    globe::World seen_;
    std::uint64_t synced_ = 0;
    int ticks_ = 0;
    bool syncing_ = false;
    std::map<net::ConnectionId, std::unique_ptr<Player>> players_;
};

} // namespace hog::server
