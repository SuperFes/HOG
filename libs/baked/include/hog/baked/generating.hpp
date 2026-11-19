#pragma once

#include <hog/baked/chunk_filling.hpp>
#include <hog/baked/chunk_store.hpp>
#include <hog/baked/wants_table.hpp>
#include <hog/core/result.hpp>
#include <hog/globe/generator.hpp>
#include <hog/jobs/job_system.hpp>

#include <chrono>
#include <functional>
#include <set>
#include <stop_token>

namespace hog::baked {

// Fills what the wants table asks for into the store, highest ranked first: each want new to it
// is handed to a filler, each gone from the table forgotten, and each the store turns out to hold
// all of logged as answered. Readers learn what was written from the store's log, so this may run
// in another process from them as well as on a thread of their own.
class Generating {
public:
    Generating(const globe::Generator& generator, ChunkStore& store, const WantsTable& wants,
               jobs::JobSystem& jobs, ChunkFiller::Options options = {});

    // Takes in the table as it is now, writes the fills finished and starts those next. With
    // `wait`, first waits for a fill under way. Whether anything's still being filled.
    [[nodiscard]] Result<bool> step(bool wait);

    // Steps until `stop`, resting `rest` between looks at the table while nothing's being
    // filled. Each step that fails is told to `failed`, and the next goes on after a rest.
    void run(const std::stop_token& stop, const std::function<void(const Error&)>& failed,
             std::chrono::milliseconds rest = std::chrono::milliseconds{50});

private:
    ChunkStore* store_;
    const WantsTable* wants_;
    ChunkFiller filler_;
    // The asks handed to the filler and not forgotten, answered or not.
    std::set<ChunkFiller::Asked> handed_;
};

} // namespace hog::baked
