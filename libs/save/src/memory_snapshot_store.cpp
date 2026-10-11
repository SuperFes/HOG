#include <hog/save/memory_snapshot_store.hpp>

#include <algorithm>
#include <format>
#include <iterator>
#include <ranges>

namespace hog::save {

Result<SnapshotStore::Id> MemorySnapshotStore::write(GameTime time,
                                                     std::span<const std::byte> encoded) {
    stored_.push_back(Stored{.id = next_, .time = time, .bytes = {encoded.begin(), encoded.end()}});
    return next_++;
}

Result<std::vector<SnapshotStore::Id>> MemorySnapshotStore::list() const {
    std::vector<Id> ids;
    std::ranges::copy(stored_ | std::views::reverse | std::views::transform(&Stored::id),
                      std::back_inserter(ids));
    return ids;
}

Result<std::vector<std::byte>> MemorySnapshotStore::read(Id id) const {
    const auto found = std::ranges::find(stored_, id, &Stored::id);
    if (found == stored_.end()) {
        return make_error(ErrorCode::NotFound, std::format("no snapshot {}", id));
    }
    return found->bytes;
}

Result<std::size_t> MemorySnapshotStore::prune(std::size_t keep) {
    const std::size_t removed = stored_.size() > keep ? stored_.size() - keep : 0;
    stored_.erase(stored_.begin(), stored_.begin() + static_cast<std::ptrdiff_t>(removed));
    return removed;
}

} // namespace hog::save
