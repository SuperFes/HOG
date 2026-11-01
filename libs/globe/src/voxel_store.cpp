#include <hog/core/assert.hpp>
#include <hog/globe/voxel_store.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <unordered_set>
#include <utility>
#include <vector>

namespace hog::globe {

VoxelStore::VoxelStore(const Layers& layers) : layers_{layers} {
    HOG_ASSERT(layers.level >= 3 && layers.level <= Cell::max_level,
               "a face must be at least a brick across");
    HOG_ASSERT(layers.count > 0 && std::cmp_less((layers.count - 1) / brick_side, most_bricks_up),
               "too many layers to store");
}

std::uint32_t VoxelStore::bricks_across() const noexcept {
    return (std::uint32_t{1} << layers_.level) / brick_side;
}

std::uint32_t VoxelStore::bricks_up() const noexcept {
    return (static_cast<std::uint32_t>(layers_.count) + brick_side - 1) / brick_side;
}

bool VoxelStore::holds_site(BrickSite brick) const noexcept {
    return brick.face < 6 && brick.i < bricks_across() && brick.j < bricks_across() &&
           brick.layer < bricks_up();
}

std::optional<Voxel> VoxelStore::at(Site site) const {
    const Brick* held = brick(brick_of(site));
    if (held == nullptr) {
        return std::nullopt;
    }
    return held->at(index_in_brick(site));
}

const Brick* VoxelStore::brick(BrickSite site) const {
    HOG_ASSERT(holds_site(site), "brick out of range");
    const auto found = bricks_.find(brick_key(site));
    return found == bricks_.end() ? nullptr : &found->second;
}

void VoxelStore::put(BrickSite site, Brick brick) {
    HOG_ASSERT(holds_site(site), "brick out of range");
    const std::uint64_t key = brick_key(site);
    bricks_.insert_or_assign(key, std::move(brick));
    changed_.insert(key);
    log_.emplace_back(++revision_, key);
}

void VoxelStore::erase(BrickSite site) {
    HOG_ASSERT(holds_site(site), "brick out of range");
    const std::uint64_t key = brick_key(site);
    if (bricks_.erase(key) > 0) {
        changed_.insert(key);
        log_.emplace_back(++revision_, key);
    }
}

std::size_t VoxelStore::bytes() const noexcept {
    // Each entry of the table: its key and brick, and a node's pointer and hash.
    std::size_t total = bricks_.bucket_count() * sizeof(void*);
    for (const auto& [key, brick] : bricks_) {
        total += sizeof(key) + brick.bytes() + (2 * sizeof(void*));
    }
    return total;
}

namespace {

std::vector<BrickSite> in_order(std::vector<std::uint64_t> keys) {
    std::ranges::sort(keys);
    std::vector<BrickSite> sites;
    sites.reserve(keys.size());
    std::ranges::transform(keys, std::back_inserter(sites), &brick_at_key);
    return sites;
}

} // namespace

std::vector<BrickSite> VoxelStore::sites() const {
    std::vector<std::uint64_t> keys;
    keys.reserve(bricks_.size());
    for (const auto& [key, brick] : bricks_) {
        keys.push_back(key);
    }
    return in_order(std::move(keys));
}

std::vector<BrickSite> VoxelStore::changed() const {
    return in_order({changed_.begin(), changed_.end()});
}

std::vector<BrickSite> VoxelStore::changed_since(std::uint64_t revision) const {
    std::unordered_set<std::uint64_t> keys;
    for (auto entry = log_.rbegin(); entry != log_.rend() && entry->first > revision; ++entry) {
        keys.insert(entry->second);
    }
    return in_order({keys.begin(), keys.end()});
}

} // namespace hog::globe
