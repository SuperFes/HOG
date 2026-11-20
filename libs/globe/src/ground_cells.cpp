#include "ground_cells.hpp"

#include <hog/core/hash.hpp>

#include <algorithm>
#include <bit>
#include <utility>

namespace hog::globe {

GroundCells::GroundCells(std::size_t most) : per_shard_{std::max<std::size_t>(most / shards, 1)} {}

std::size_t GroundCells::Hash::operator()(const Key& key) const noexcept {
    return static_cast<std::size_t>(hash_values(index_of(key.cell), key.cell.level,
                                                std::bit_cast<std::uint64_t>(key.lod),
                                                static_cast<std::uint8_t>(key.crags)));
}

GroundCells::Shard& GroundCells::shard_of(const Key& key) const noexcept {
    return shards_.at(Hash{}(key) % shards);
}

void GroundCells::hold(Shard& shard, std::size_t most, const Key& key,
                       const terms::Ground& ground) {
    if (shard.fresh.size() >= most) {
        shard.old = std::exchange(shard.fresh, {});
    }
    shard.fresh.insert_or_assign(key, ground);
}

std::optional<terms::Ground> GroundCells::find(const Key& key) const {
    Shard& shard = shard_of(key);
    const std::scoped_lock lock{shard.mutex};
    if (const auto found = shard.fresh.find(key); found != shard.fresh.end()) {
        return found->second;
    }
    const auto found = shard.old.find(key);
    if (found == shard.old.end()) {
        return std::nullopt;
    }
    const terms::Ground ground = found->second;
    shard.old.erase(found);
    hold(shard, per_shard_, key, ground);
    return ground;
}

void GroundCells::keep(const Key& key, const terms::Ground& ground) const {
    Shard& shard = shard_of(key);
    const std::scoped_lock lock{shard.mutex};
    hold(shard, per_shard_, key, ground);
}

std::size_t GroundCells::size() const {
    std::size_t held = 0;
    for (Shard& shard : shards_) {
        const std::scoped_lock lock{shard.mutex};
        held += shard.fresh.size() + shard.old.size();
    }
    return held;
}

} // namespace hog::globe
