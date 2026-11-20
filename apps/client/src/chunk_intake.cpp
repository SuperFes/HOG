#include <hog/client/chunk_intake.hpp>

#include <algorithm>
#include <iterator>
#include <utility>

namespace hog::client {
namespace {

// Chunks decoded by one job at most, so a big batch is spread over the jobs.
constexpr std::size_t slice = 128;

// Decodes `chunks` in order; those from the cache noted for the server to check.
ChunkIntake::Taken decoded(const std::vector<protocol::SentChunk>& chunks, bool kept) {
    ChunkIntake::Taken taken;
    taken.chunks.reserve(chunks.size());
    for (const protocol::SentChunk& chunk : chunks) {
        auto made = decode_chunk(chunk);
        if (!made) {
            taken.failed.push_back(made.error());
            continue;
        }
        taken.chunks.push_back(std::move(*made));
        if (kept) {
            taken.kept.push_back({.kind = chunk.kind,
                                  .band = chunk.band,
                                  .depth = chunk.depth,
                                  .x = chunk.x,
                                  .y = chunk.y,
                                  .z = chunk.z,
                                  .version = chunk.version});
        }
    }
    return taken;
}

void append(ChunkIntake::Taken& into, ChunkIntake::Taken from) {
    into.chunks.insert(into.chunks.end(), std::make_move_iterator(from.chunks.begin()),
                       std::make_move_iterator(from.chunks.end()));
    into.kept.insert(into.kept.end(), from.kept.begin(), from.kept.end());
    into.looked.insert(into.looked.end(), from.looked.begin(), from.looked.end());
    into.failed.insert(into.failed.end(), std::make_move_iterator(from.failed.begin()),
                       std::make_move_iterator(from.failed.end()));
}

} // namespace

ChunkIntake::ChunkIntake(jobs::JobSystem& jobs, std::optional<ChunkCache> cache)
    : jobs_{&jobs}, cache_{cache ? std::make_shared<ChunkCache>(std::move(*cache)) : nullptr} {}

ChunkIntake::~ChunkIntake() {
    Taken ignored;
    gather(true, ignored);
}

void ChunkIntake::sent(std::vector<protocol::SentChunk> chunks) {
    if (chunks.empty()) {
        return;
    }
    for (std::size_t from = 0; from < chunks.size(); from += slice) {
        const auto to = std::min(chunks.size(), from + slice);
        steps_.push_back(
            {.sent = {std::make_move_iterator(chunks.begin() + static_cast<std::ptrdiff_t>(from)),
                      std::make_move_iterator(chunks.begin() + static_cast<std::ptrdiff_t>(to))}});
        if (cache_) {
            // Kept as they're decoded, in order with what's looked up.
            Step keep{.uses_cache = true, .keep = true};
            keep.sent = steps_.back().sent;
            steps_.push_back(std::move(keep));
        }
    }
    start();
}

void ChunkIntake::look_up(std::vector<ChunkCache::Wanted> wanted) {
    if (wanted.empty()) {
        return;
    }
    if (!cache_) {
        Step none;
        none.wanted = std::move(wanted);
        steps_.push_back(std::move(none));
    } else {
        steps_.push_back({.uses_cache = true, .wanted = std::move(wanted)});
    }
    start();
}

void ChunkIntake::start() {
    bool cache_busy = std::ranges::any_of(
        steps_, [](const Step& step) { return step.uses_cache && step.job && !step.job->done(); });
    for (Step& step : steps_) {
        if (step.job) {
            continue;
        }
        if (step.uses_cache) {
            // Never two at the cache at once, and never one past another waiting for it.
            if (cache_busy) {
                continue;
            }
            cache_busy = true;
            if (step.keep) {
                step.job = std::make_unique<jobs::Async<Taken>>(
                    *jobs_, [cache = cache_, chunks = std::move(step.sent)] {
                        Taken taken;
                        if (auto kept = cache->keep(chunks); !kept) {
                            taken.failed.push_back(kept.error());
                        }
                        return taken;
                    });
            } else {
                step.job = std::make_unique<jobs::Async<Taken>>(
                    *jobs_, [cache = cache_, wanted = step.wanted] {
                        auto read = cache->read(wanted);
                        if (!read) {
                            Taken taken;
                            taken.failed.push_back(read.error());
                            taken.looked = wanted;
                            return taken;
                        }
                        Taken taken = decoded(*read, true);
                        taken.looked = wanted;
                        return taken;
                    });
            }
            continue;
        }
        if (!step.wanted.empty()) {
            // Nothing kept to look up in.
            step.job = std::make_unique<jobs::Async<Taken>>(*jobs_, [wanted = step.wanted] {
                Taken taken;
                taken.looked = wanted;
                return taken;
            });
            continue;
        }
        step.job = std::make_unique<jobs::Async<Taken>>(
            *jobs_, [chunks = std::move(step.sent)] { return decoded(chunks, false); });
    }
}

void ChunkIntake::gather(bool wait, Taken& into) {
    while (!steps_.empty()) {
        Step& front = steps_.front();
        if (!front.job) {
            if (!wait) {
                return;
            }
            start();
            continue;
        }
        if (!wait && !front.job->done()) {
            return;
        }
        append(into, front.job->take());
        steps_.pop_front();
        start();
    }
}

ChunkIntake::Taken ChunkIntake::take() {
    start();
    Taken taken;
    gather(false, taken);
    return taken;
}

ChunkIntake::Taken ChunkIntake::finish() {
    Taken taken;
    gather(true, taken);
    return taken;
}

} // namespace hog::client
