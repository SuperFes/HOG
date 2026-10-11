#pragma once

#include <hog/core/result.hpp>
#include <hog/planet/change.hpp>

#include <cstddef>
#include <span>
#include <vector>

namespace hog::planet {

// Durable storage for a world's changes. Backends are chosen at runtime, so this is an interface;
// every call handles a whole batch.
class ChangeStore {
public:
    ChangeStore() = default;
    virtual ~ChangeStore() = default;

    // Appends the changes in order, all or none.
    virtual Result<void> append(std::span<const Change> changes) = 0;

    // Every stored change, in the order appended.
    [[nodiscard]] virtual Result<std::vector<Change>> load() const = 0;

    // Removes the matching changes; returns how many.
    virtual Result<std::size_t> erase(const ChangeFilter& filter) = 0;

protected:
    ChangeStore(const ChangeStore&) = default;
    ChangeStore(ChangeStore&&) = default;
    ChangeStore& operator=(const ChangeStore&) = default;
    ChangeStore& operator=(ChangeStore&&) = default;
};

} // namespace hog::planet
