#pragma once

#include <hog/save/snapshot_store.hpp>

#include <cstddef>
#include <vector>

namespace hog::save {

// Snapshots kept in memory, for tests and worlds that are never saved to disk.
class MemorySnapshotStore final : public SnapshotStore {
public:
    Result<Id> write(GameTime time, std::span<const std::byte> encoded) override;
    [[nodiscard]] Result<std::vector<Id>> list() const override;
    [[nodiscard]] Result<std::vector<std::byte>> read(Id id) const override;
    Result<std::size_t> prune(std::size_t keep) override;

    // Stored bytes, oldest first, open to tests that damage them.
    struct Stored {
        Id id = 0;
        GameTime time;
        std::vector<std::byte> bytes;
    };
    [[nodiscard]] std::vector<Stored>& stored() noexcept { return stored_; }

private:
    std::vector<Stored> stored_;
    Id next_ = 1;
};

} // namespace hog::save
