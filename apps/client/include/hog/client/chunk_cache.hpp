#pragma once

#include <hog/chunks/globe_stream.hpp>
#include <hog/chunks/mesh_codec.hpp>
#include <hog/core/time.hpp>
#include <hog/protocol/messages.hpp>
#include <hog/terrain/block_mesh.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace hog::client {

// The surfaces of the planet the client holds, and what of them it draws. The server sends a plan
// of the chunks wanted round the eye; the client draws the plan from the chunks it holds, asks for
// those it lacks, nearest first, and keeps what it's been sent for as long as it has room, the
// least recently planned going first. A chunk stays drawn until what the plan has in its place is
// there to be drawn, so the ground doesn't open; and one the plan wants and the client lacks is
// drawn from finer chunks it holds there, if they cover it.
class ChunkCache {
public:
    // Holds at most `most` chunks.
    explicit ChunkCache(std::size_t most) noexcept : most_(most) {}

    // The server's plan: what's wanted now, nearest first.
    void plan(const protocol::ChunkPlan& plan);
    // A part of a chunk the server sent.
    void receive(protocol::Chunk part);

    // Chunks of the plan to ask for, nearest first, as many as there's room to wait on: those not
    // held at their planned version, and not asked for lately.
    [[nodiscard]] std::vector<std::uint64_t> to_ask(MonotonicClock::time_point now);

    // What to show and take away to draw the plan from what's held, from the last call.
    [[nodiscard]] chunks::GlobeStream::Changes changes();

    // Whether every chunk of the plan is held, at its planned version.
    [[nodiscard]] bool complete() const;
    // Chunks in the plan, held, and drawn.
    [[nodiscard]] std::size_t planned() const noexcept { return planned_.size(); }
    [[nodiscard]] std::size_t held() const noexcept { return held_.size(); }
    [[nodiscard]] std::size_t drawn() const noexcept { return drawn_.size(); }

    // Chunks asked for at a time at most, and how long before one's asked for again.
    static constexpr std::size_t window = 24;
    static constexpr std::chrono::seconds ask_again{8};

private:
    struct Held {
        std::uint32_t version = 0;
        std::shared_ptr<const terrain::BlockMesh> ground;
        std::shared_ptr<const terrain::BlockMesh> water;
        std::uint64_t used = 0; // when it was last in the plan, counted in plans
    };

    // The chunks to draw for the plan: planned ones held, and finer ones held where one isn't; and
    // the planned ones that has made ready to draw, one way or the other.
    [[nodiscard]] std::set<std::uint64_t> to_draw(std::set<std::uint64_t>& ready) const;
    // The keys of finer chunks held that cover `chunk`, if they do.
    [[nodiscard]] bool covered(std::uint64_t key, int depth, std::set<std::uint64_t>& into) const;
    void evict();

    std::size_t most_;
    std::unordered_map<std::uint64_t, Held> held_;
    std::vector<protocol::PlannedChunk> planned_;
    // Planned keys, and for every chunk with one under it, those planned under it.
    std::unordered_set<std::uint64_t> in_plan_;
    std::unordered_map<std::uint64_t, std::vector<std::uint64_t>> under_;
    std::unordered_map<std::uint64_t, MonotonicClock::time_point> asked_;
    chunks::ChunkParts parts_;
    // What's drawn, at what version.
    std::map<std::uint64_t, std::uint32_t> drawn_;
    std::uint64_t plans_ = 0;
};

} // namespace hog::client
