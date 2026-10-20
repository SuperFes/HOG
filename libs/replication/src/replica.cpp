#include <hog/replication/replica.hpp>

#include <algorithm>
#include <utility>

namespace hog::replication {

namespace {

const EntityStates& nothing() {
    static const EntityStates none;
    return none;
}

} // namespace

Result<bool> Replica::receive(const protocol::Snapshot& snapshot) {
    if (!held_.empty() && snapshot.tick <= held_.back().first) {
        return false;
    }
    const EntityStates* baseline = &nothing();
    if (snapshot.baseline) {
        const auto found = std::ranges::find(held_, *snapshot.baseline,
                                             [](const auto& entry) { return entry.first; });
        if (found == held_.end()) {
            return false;
        }
        baseline = &found->second;
    }
    auto states = replication::apply(*baseline, snapshot);
    if (!states) {
        return std::unexpected(std::move(states.error()));
    }
    held_.emplace_back(snapshot.tick, std::move(*states));
    if (held_.size() > kept) {
        held_.pop_front();
    }
    return true;
}

const EntityStates& Replica::now() const noexcept {
    return held_.empty() ? nothing() : held_.back().second;
}

std::optional<std::uint32_t> Replica::tick() const noexcept {
    if (held_.empty()) {
        return std::nullopt;
    }
    return held_.back().first;
}

} // namespace hog::replication
