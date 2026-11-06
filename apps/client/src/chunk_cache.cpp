#include <hog/client/chunk_cache.hpp>
#include <hog/globe/cell.hpp>

#include <algorithm>
#include <limits>

namespace hog::client {

void ChunkCache::plan(const protocol::ChunkPlan& plan) {
    planned_ = plan.wanted;
    ++plans_;
    in_plan_.clear();
    under_.clear();
    for (const protocol::PlannedChunk& planned : planned_) {
        in_plan_.insert(planned.key);
        for (globe::Cell over = chunks::globe_cell(planned.key); over.level > 0;) {
            over = globe::parent(over);
            under_[chunks::globe_key(over)].push_back(planned.key);
        }
        if (const auto found = held_.find(planned.key); found != held_.end()) {
            found->second.used = plans_;
        }
    }
    evict();
}

void ChunkCache::receive(protocol::Chunk part) {
    const std::uint64_t key = part.key;
    const std::uint32_t version = part.version;
    const auto whole = parts_.add(std::move(part));
    if (!whole) {
        return;
    }
    asked_.erase(key);
    const auto have = held_.find(key);
    // An older meshing coming late doesn't replace a newer.
    if (have != held_.end() && have->second.version > version) {
        return;
    }
    held_.insert_or_assign(
        key,
        Held{.version = version,
             .ground = std::make_shared<const terrain::BlockMesh>(chunks::from_sent(whole->ground)),
             .water = std::make_shared<const terrain::BlockMesh>(chunks::from_sent(whole->water)),
             .used = plans_});
    evict();
}

std::vector<std::uint64_t> ChunkCache::to_ask(MonotonicClock::time_point now) {
    // Those asked for and not come, within the time they're waited for.
    std::erase_if(asked_, [&](const auto& asked) { return now - asked.second >= ask_again; });
    std::vector<std::uint64_t> keys;
    for (const protocol::PlannedChunk& planned : planned_) {
        if (asked_.size() + keys.size() >= window) {
            break;
        }
        const auto have = held_.find(planned.key);
        const bool current =
            have != held_.end() && planned.version > 0 && have->second.version >= planned.version;
        // Held and not yet meshed again, there's nothing newer to ask for.
        const bool nothing_newer = have != held_.end() && planned.version == 0;
        if (current || nothing_newer || asked_.contains(planned.key)) {
            continue;
        }
        keys.push_back(planned.key);
    }
    for (const std::uint64_t key : keys) {
        asked_.emplace(key, now);
    }
    return keys;
}

bool ChunkCache::covered(std::uint64_t key, int depth, std::set<std::uint64_t>& into) const {
    if (depth == 0) {
        return false;
    }
    const globe::Cell chunk = chunks::globe_cell(key);
    if (chunk.level >= globe::Cell::max_level) {
        return false;
    }
    std::set<std::uint64_t> found;
    for (const globe::Cell child : globe::children(chunk)) {
        const std::uint64_t child_key = chunks::globe_key(child);
        if (held_.contains(child_key)) {
            found.insert(child_key);
        } else if (!covered(child_key, depth - 1, found)) {
            return false;
        }
    }
    into.insert(found.begin(), found.end());
    return true;
}

std::set<std::uint64_t> ChunkCache::to_draw(std::set<std::uint64_t>& ready) const {
    std::set<std::uint64_t> draw;
    for (const protocol::PlannedChunk& planned : planned_) {
        if (held_.contains(planned.key)) {
            draw.insert(planned.key);
            ready.insert(planned.key);
        } else {
            // Not there yet: finer chunks held in its place, if they cover it.
            std::set<std::uint64_t> finer;
            if (covered(planned.key, 3, finer)) {
                draw.insert(finer.begin(), finer.end());
                ready.insert(planned.key);
            }
        }
    }
    return draw;
}

chunks::GlobeStream::Changes ChunkCache::changes() {
    chunks::GlobeStream::Changes out;
    std::set<std::uint64_t> ready;
    std::set<std::uint64_t> draw = to_draw(ready);
    // What's drawn and isn't wanted stays while anything planned over or under it isn't drawn yet.
    for (const auto& [key, version] : drawn_) {
        if (draw.contains(key) || !held_.contains(key)) {
            continue;
        }
        bool waiting = false;
        const auto pending = [&](std::uint64_t planned) { return !ready.contains(planned); };
        for (globe::Cell over = chunks::globe_cell(key); over.level > 0 && !waiting;) {
            over = globe::parent(over);
            const std::uint64_t over_key = chunks::globe_key(over);
            waiting = in_plan_.contains(over_key) && pending(over_key);
        }
        if (!waiting) {
            if (const auto below = under_.find(key); below != under_.end()) {
                waiting = std::ranges::any_of(below->second, pending);
            }
        }
        if (waiting) {
            draw.insert(key);
        }
    }
    for (const std::uint64_t key : draw) {
        const Held& held = held_.at(key);
        const auto drawn = drawn_.find(key);
        if (drawn == drawn_.end() || drawn->second != held.version) {
            out.shown.push_back({.key = key, .ground = held.ground, .water = held.water});
            drawn_.insert_or_assign(key, held.version);
        }
    }
    for (auto it = drawn_.begin(); it != drawn_.end();) {
        if (draw.contains(it->first)) {
            ++it;
        } else {
            out.gone.push_back(it->first);
            it = drawn_.erase(it);
        }
    }
    return out;
}

bool ChunkCache::complete() const {
    return !planned_.empty() &&
           std::ranges::all_of(planned_, [&](const protocol::PlannedChunk& planned) {
               const auto have = held_.find(planned.key);
               return have != held_.end() && planned.version > 0 &&
                      have->second.version >= planned.version;
           });
}

void ChunkCache::evict() {
    while (held_.size() > most_) {
        // The least recently planned, of those neither wanted nor drawn.
        auto oldest = held_.end();
        for (auto it = held_.begin(); it != held_.end(); ++it) {
            if (in_plan_.contains(it->first) || drawn_.contains(it->first)) {
                continue;
            }
            if (oldest == held_.end() || it->second.used < oldest->second.used) {
                oldest = it;
            }
        }
        if (oldest == held_.end()) {
            return;
        }
        held_.erase(oldest);
    }
}

} // namespace hog::client
