#include <hog/core/assert.hpp>
#include <hog/globe/voxel_store.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <utility>
#include <vector>

namespace hog::globe {
namespace {

// A brick's site packs into a key: the face in 3 bits, i and j in 21 each, the layer in 19.
constexpr unsigned across_bits = 21;
constexpr unsigned up_bits = 19;
constexpr unsigned j_shift = up_bits;
constexpr unsigned i_shift = j_shift + across_bits;
constexpr unsigned face_shift = i_shift + across_bits;

} // namespace

VoxelStore::VoxelStore(const Layers& layers) : layers_{layers} {
    HOG_ASSERT(layers.level >= 3 && layers.level <= Cell::max_level,
               "a face must be at least a brick across");
    HOG_ASSERT(layers.count > 0 &&
                   std::cmp_less((layers.count - 1) / brick_side, std::uint32_t{1} << up_bits),
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

std::uint64_t VoxelStore::key_of(BrickSite site) noexcept {
    return (std::uint64_t{site.face} << face_shift) | (std::uint64_t{site.i} << i_shift) |
           (std::uint64_t{site.j} << j_shift) | site.layer;
}

BrickSite VoxelStore::site_of(std::uint64_t key) noexcept {
    const auto field = [&](unsigned shift, unsigned width) {
        return static_cast<std::uint32_t>((key >> shift) & ((std::uint64_t{1} << width) - 1));
    };
    return {.face = static_cast<std::uint8_t>(key >> face_shift),
            .i = field(i_shift, across_bits),
            .j = field(j_shift, across_bits),
            .layer = field(0, up_bits)};
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
    const auto found = bricks_.find(key_of(site));
    return found == bricks_.end() ? nullptr : &found->second;
}

void VoxelStore::put(BrickSite site, Brick brick) {
    HOG_ASSERT(holds_site(site), "brick out of range");
    const std::uint64_t key = key_of(site);
    bricks_.insert_or_assign(key, std::move(brick));
    changed_.insert(key);
}

void VoxelStore::erase(BrickSite site) {
    HOG_ASSERT(holds_site(site), "brick out of range");
    const std::uint64_t key = key_of(site);
    if (bricks_.erase(key) > 0) {
        changed_.insert(key);
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

template <typename Keys>
std::vector<BrickSite> in_order(const Keys& keys, auto key_of, auto site_of) {
    std::vector<std::uint64_t> sorted;
    sorted.reserve(keys.size());
    for (const auto& entry : keys) {
        sorted.push_back(key_of(entry));
    }
    std::ranges::sort(sorted);
    std::vector<BrickSite> sites;
    sites.reserve(sorted.size());
    std::ranges::transform(sorted, std::back_inserter(sites), site_of);
    return sites;
}

} // namespace

std::vector<BrickSite> VoxelStore::sites() const {
    return in_order(bricks_, [](const auto& entry) { return entry.first; }, &site_of);
}

std::vector<BrickSite> VoxelStore::take_changed() {
    std::vector<BrickSite> sites =
        in_order(changed_, [](std::uint64_t key) { return key; }, &site_of);
    changed_.clear();
    return sites;
}

} // namespace hog::globe
