#include <hog/globe/column_cache.hpp>
#include <hog/globe/world.hpp>

#include <algorithm>
#include <bit>
#include <utility>

namespace hog::globe {
namespace {

constexpr auto side = static_cast<std::uint32_t>(Volume::tile_side);

} // namespace

Result<void> ColumnCache::take(std::uint8_t face, std::uint32_t i, std::uint32_t j,
                               std::uint8_t level, std::span<const std::byte> bytes) {
    const Site corner{.face = face, .i = i * side, .j = j * side};
    if (level == 0) {
        if (bytes.empty()) {
            volume_->drop_column(corner);
        } else if (auto decoded = volume_->decode_column(corner, bytes); !decoded) {
            return decoded;
        }
        coarse_.erase(corner);
    } else {
        if (level > std::countr_zero(side)) {
            return make_error(ErrorCode::OutOfRange, "a tile column coarser than a voxel a tile");
        }
        if (bytes.empty()) {
            coarse_.erase(corner);
        } else {
            auto decoded = TileColumn::decode(bytes, Volume::tile_side >> level);
            if (!decoded) {
                return std::unexpected(std::move(decoded).error());
            }
            coarse_.insert_or_assign(corner, Coarse{.level = level, .column = std::move(*decoded)});
        }
        volume_->drop_column(corner);
    }
    changed_.push_back(corner);
    return {};
}

const ColumnCache::Coarse* ColumnCache::coarse(Site corner) const {
    const auto found = coarse_.find(corner);
    return found == coarse_.end() ? nullptr : &found->second;
}

void ColumnCache::keep_near(const Layers& layers, DVec3 at, double metres) {
    const std::vector<Site> near = tile_columns_near(layers, at, metres);
    for (const Site corner : volume_->columns()) {
        if (!std::ranges::binary_search(near, corner)) {
            volume_->drop_column(corner);
            changed_.push_back(corner);
        }
    }
    for (auto at_corner = coarse_.begin(); at_corner != coarse_.end();) {
        if (std::ranges::binary_search(near, at_corner->first)) {
            ++at_corner;
        } else {
            changed_.push_back(at_corner->first);
            at_corner = coarse_.erase(at_corner);
        }
    }
}

std::vector<Site> ColumnCache::take_changed() {
    std::vector<Site> changed = std::exchange(changed_, {});
    std::ranges::sort(changed);
    const auto [first, last] = std::ranges::unique(changed);
    changed.erase(first, last);
    return changed;
}

} // namespace hog::globe
