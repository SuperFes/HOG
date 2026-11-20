#pragma once

#include <hog/client/chunk_cache.hpp>
#include <hog/client/held_pieces.hpp>
#include <hog/core/result.hpp>
#include <hog/jobs/job_system.hpp>
#include <hog/protocol/messages.hpp>

#include <cstddef>
#include <deque>
#include <memory>
#include <optional>
#include <vector>

namespace hog::client {

// Chunks decoded off the thread that draws, so a frame never waits on decoding them or on the
// disk: those the server sends, kept in the cache as they come, and those looked up in the cache.
// All on the jobs, the cache used by one at a time; what's handed over comes back decoded in the
// order it was handed over. Used by the thread that made the jobs.
class ChunkIntake {
public:
    // The jobs must outlive it.
    ChunkIntake(jobs::JobSystem& jobs, std::optional<ChunkCache> cache);
    ChunkIntake(const ChunkIntake&) = delete;
    ChunkIntake& operator=(const ChunkIntake&) = delete;
    ChunkIntake(ChunkIntake&&) = delete;
    ChunkIntake& operator=(ChunkIntake&&) = delete;
    ~ChunkIntake();

    [[nodiscard]] bool caching() const noexcept { return cache_ != nullptr; }

    // Has chunks the server sent decoded, and kept.
    void sent(std::vector<protocol::SentChunk> chunks);
    // Has what the cache keeps of these looked up and decoded.
    void look_up(std::vector<ChunkCache::Wanted> wanted);

    struct Taken {
        // In the order handed over.
        std::vector<DecodedChunk> chunks;
        // Of those, the ones read from the cache, for the server to check.
        std::vector<protocol::CheckedChunk> kept;
        // What was looked up, done.
        std::vector<ChunkCache::Wanted> looked;
        // Why any couldn't be decoded, read or kept.
        std::vector<Error> failed;
    };
    // Starts what can start, and takes what's done since last asked without waiting.
    [[nodiscard]] Taken take();
    // Waits for all that's handed over, and takes it.
    [[nodiscard]] Taken finish();
    // What's handed over and not yet taken.
    [[nodiscard]] std::size_t waiting() const noexcept { return steps_.size(); }

private:
    // A piece of work, started once it may be: decoding chunks sent, any number at once, or
    // using the cache, one at a time.
    struct Step {
        bool uses_cache = false;
        std::vector<protocol::SentChunk> sent{};   // NOLINT(readability-redundant-member-init)
        std::vector<ChunkCache::Wanted> wanted{};  // NOLINT(readability-redundant-member-init)
        bool keep = false;                         // the chunks sent, into the cache
        std::unique_ptr<jobs::Async<Taken>> job{}; // NOLINT(readability-redundant-member-init)
    };

    void start();
    void gather(bool wait, Taken& into);

    jobs::JobSystem* jobs_;
    std::shared_ptr<ChunkCache> cache_;
    std::deque<Step> steps_;
};

} // namespace hog::client
