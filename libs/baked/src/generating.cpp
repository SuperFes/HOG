#include <hog/baked/generating.hpp>

#include <algorithm>
#include <thread>
#include <vector>

namespace hog::baked {

Generating::Generating(const globe::Generator& generator, ChunkStore& store,
                       const WantsTable& wants, jobs::JobSystem& jobs, ChunkFiller::Options options)
    : store_{&store}, wants_{&wants}, filler_{generator, store, jobs, options} {}

Result<bool> Generating::step(bool wait) {
    auto wants = wants_->read();
    if (!wants) {
        return std::unexpected{wants.error()};
    }
    const std::uint8_t finest = store_->baking().grid.depth();
    std::set<ChunkFiller::Asked> current;
    for (const WantsTable::Want& want : *wants) {
        if (!want.node.is_valid() || want.node.depth > finest) {
            continue;
        }
        const ChunkFiller::Asked ask{.node = want.node,
                                     .depth = std::clamp(want.depth, want.node.depth, finest)};
        current.insert(ask);
        if (handed_.insert(ask).second) {
            filler_.want(ask.node, ask.depth, want.rank);
        }
    }
    std::vector<ChunkFiller::Asked> gone;
    std::ranges::set_difference(handed_, current, std::back_inserter(gone));
    if (!gone.empty()) {
        filler_.forget(gone);
        for (const ChunkFiller::Asked& ask : gone) {
            handed_.erase(ask);
        }
    }
    if (auto pumped = filler_.pump(wait); !pumped) {
        return std::unexpected{pumped.error()};
    }
    if (const std::vector<ChunkFiller::Asked> answered = filler_.take_answered();
        !answered.empty()) {
        const Result<void> logged = store_->write([&](ChunkStore::Batch& batch) -> Result<void> {
            for (const ChunkFiller::Asked& ask : answered) {
                if (Result<void> told = batch.answered(ask.node, ask.depth); !told) {
                    return told;
                }
            }
            return {};
        });
        if (!logged) {
            return std::unexpected{logged.error()};
        }
    }
    return !filler_.idle();
}

void Generating::run(const std::stop_token& stop, const std::function<void(const Error&)>& failed,
                     std::chrono::milliseconds rest) {
    bool busy = false;
    while (!stop.stop_requested()) {
        const Result<bool> stepped = step(busy);
        if (!stepped) {
            failed(stepped.error());
            busy = false;
        } else {
            busy = *stepped;
        }
        if (!busy) {
            std::this_thread::sleep_for(rest);
        }
    }
}

} // namespace hog::baked
