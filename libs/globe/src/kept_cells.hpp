#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>

namespace hog::globe {

// What's made for a cell, kept once made for whoever reads it next, on any thread. What's kept is
// what making it afresh gives, so keeping it never changes a read. About `most` of those read last
// are kept, never more than twice that.
template <typename Key, typename Value, typename Hash>
class KeptCells {
public:
    explicit KeptCells(std::size_t most) : per_shard_{std::max<std::size_t>(most / shards, 1)} {}

    [[nodiscard]] std::optional<Value> find(const Key& key) const {
        Shard& shard = shard_of(key);
        const std::scoped_lock lock{shard.mutex};
        if (const auto found = shard.fresh.find(key); found != shard.fresh.end()) {
            return found->second;
        }
        const auto found = shard.old.find(key);
        if (found == shard.old.end()) {
            return std::nullopt;
        }
        Value value = std::move(found->second);
        shard.old.erase(found);
        hold(shard, per_shard_, key, value);
        return value;
    }

    void keep(const Key& key, const Value& value) const {
        Shard& shard = shard_of(key);
        const std::scoped_lock lock{shard.mutex};
        hold(shard, per_shard_, key, value);
    }

    [[nodiscard]] std::size_t size() const {
        std::size_t held = 0;
        for (Shard& shard : shards_) {
            const std::scoped_lock lock{shard.mutex};
            held += shard.fresh.size() + shard.old.size();
        }
        return held;
    }

private:
    using Held = std::unordered_map<Key, Value, Hash>;
    // Those read since the last turn over, and those before it, let go at the next.
    struct Shard {
        std::mutex mutex;
        Held fresh;
        Held old;
    };
    static constexpr std::size_t shards = 64;
    std::size_t per_shard_;
    mutable std::array<Shard, shards> shards_;

    [[nodiscard]] Shard& shard_of(const Key& key) const noexcept {
        return shards_.at(Hash{}(key) % shards);
    }
    static void hold(Shard& shard, std::size_t most, const Key& key, const Value& value) {
        if (shard.fresh.size() >= most) {
            shard.old = std::exchange(shard.fresh, {});
        }
        shard.fresh.insert_or_assign(key, value);
    }
};

} // namespace hog::globe
