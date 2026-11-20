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
    : jobs_{&jobs}, most_{2 * std::max<std::size_t>(1, jobs.thread_count())},
      cache_{cache ? std::make_shared<ChunkCache>(std::move(*cache)) : nullptr} {}

ChunkIntake::~ChunkIntake() {
    Taken ignored;
    gather(true, ignored);
}

void ChunkIntake::sent(std::vector<protocol::SentChunk> chunks) {
    for (std::size_t from = 0; from < chunks.size(); from += slice) {
        const auto to = std::min(chunks.size(), from + slice);
        Step decode{.sent = {chunks.begin() + static_cast<std::ptrdiff_t>(from),
                             chunks.begin() + static_cast<std::ptrdiff_t>(to)}};
        if (cache_) {
            // Kept in order with what's looked up, never holding up what's decoded.
            caching_.push_back({.sent = decode.sent, .keep = true});
        }
        decoding_.push_back(std::move(decode));
    }
    start();
}

void ChunkIntake::look_up(std::vector<ChunkCache::Wanted> wanted) {
    if (wanted.empty()) {
        return;
    }
    caching_.push_back({.wanted = std::move(wanted)});
    start();
}

std::unique_ptr<jobs::Async<ChunkIntake::Taken>> ChunkIntake::cache_job(Step& step) {
    if (!cache_) {
        // Nothing kept to look up in, nor to keep.
        return std::make_unique<jobs::Async<Taken>>(*jobs_, [wanted = std::move(step.wanted)] {
            Taken taken;
            taken.looked = wanted;
            return taken;
        });
    }
    if (step.keep) {
        return std::make_unique<jobs::Async<Taken>>(
            *jobs_, [cache = cache_, chunks = std::move(step.sent)] {
                Taken taken;
                if (auto kept = cache->keep(chunks); !kept) {
                    taken.failed.push_back(kept.error());
                }
                return taken;
            });
    }
    return std::make_unique<jobs::Async<Taken>>(*jobs_,
                                                [cache = cache_, wanted = std::move(step.wanted)] {
                                                    auto read = cache->read(wanted);
                                                    Taken taken;
                                                    if (!read) {
                                                        taken.failed.push_back(read.error());
                                                    } else {
                                                        taken = decoded(*read, true);
                                                    }
                                                    taken.looked = wanted;
                                                    return taken;
                                                });
}

void ChunkIntake::start() {
    // The cache one job at a time, in order.
    if (!caching_.empty() && !caching_.front().job && started_ < most_) {
        caching_.front().job = cache_job(caching_.front());
        ++started_;
    }
    for (Step& step : decoding_) {
        if (started_ == most_) {
            return;
        }
        if (!step.job) {
            step.job = std::make_unique<jobs::Async<Taken>>(
                *jobs_, [chunks = std::move(step.sent)] { return decoded(chunks, false); });
            ++started_;
        }
    }
}

void ChunkIntake::gather(bool wait, Taken& into) {
    for (std::deque<Step>* queue : {&decoding_, &caching_}) {
        while (!queue->empty()) {
            start();
            Step& front = queue->front();
            if (!front.job || (!wait && !front.job->done())) {
                break;
            }
            append(into, front.job->take());
            --started_;
            queue->pop_front();
        }
    }
    start();
}

ChunkIntake::Taken ChunkIntake::take() {
    Taken taken;
    gather(false, taken);
    return taken;
}

ChunkIntake::Taken ChunkIntake::finish() {
    Taken taken;
    while (waiting() > 0) {
        gather(true, taken);
    }
    return taken;
}

} // namespace hog::client
