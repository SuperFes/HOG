#pragma once

#include "terms.hpp"

#include <hog/globe/cell.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace hog::globe {

// The ground at the middles of the cube's cells, kept once made for whoever reads it next, on
// any thread. What's kept is what reading afresh gives, so keeping it never changes a read. About
// `most` of those read last are kept, never more than twice that.
class GroundCells {
public:
    static constexpr std::size_t default_most = std::size_t{1} << 17U;
    explicit GroundCells(std::size_t most = default_most);

    struct Key {
        Cell cell;
        double lod = 0.0;
        bool crags = true;
        friend bool operator==(const Key&, const Key&) noexcept = default;
    };

    [[nodiscard]] std::optional<terms::Ground> find(const Key& key) const;
    void keep(const Key& key, const terms::Ground& ground) const;
    [[nodiscard]] std::size_t size() const;

private:
    struct Hash {
        std::size_t operator()(const Key& key) const noexcept;
    };
    using Held = std::unordered_map<Key, terms::Ground, Hash>;
    // Those read since the last turn over, and those before it, let go at the next.
    struct Shard {
        std::mutex mutex;
        Held fresh;
        Held old;
    };
    static constexpr std::size_t shards = 64;
    std::size_t per_shard_;
    mutable std::array<Shard, shards> shards_;

    [[nodiscard]] Shard& shard_of(const Key& key) const noexcept;
    static void hold(Shard& shard, std::size_t most, const Key& key, const terms::Ground& ground);
};

} // namespace hog::globe
