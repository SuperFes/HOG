#pragma once

#include <hog/core/assert.hpp>

#include <algorithm>
#include <cstddef>
#include <utility>
#include <vector>

namespace hog {

// A map kept as a vector sorted by key: lookups search contiguous memory, which stays quick when
// the caches are cold, at the cost of inserting and erasing in linear time. For maps looked up far
// more often than they change. Inserting or erasing invalidates iterators and references.
template <typename Key, typename Value>
class FlatMap {
public:
    using Entry = std::pair<Key, Value>;
    using Iterator = std::vector<Entry>::iterator;
    using ConstIterator = std::vector<Entry>::const_iterator;

    [[nodiscard]] Iterator begin() noexcept { return entries_.begin(); }
    [[nodiscard]] Iterator end() noexcept { return entries_.end(); }
    [[nodiscard]] ConstIterator begin() const noexcept { return entries_.begin(); }
    [[nodiscard]] ConstIterator end() const noexcept { return entries_.end(); }
    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
    [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }

    // The first entry whose key isn't less than `key`.
    [[nodiscard]] Iterator lower_bound(const Key& key) {
        return begin() + static_cast<std::ptrdiff_t>(lower_index(key));
    }
    [[nodiscard]] ConstIterator lower_bound(const Key& key) const {
        return begin() + static_cast<std::ptrdiff_t>(lower_index(key));
    }

    [[nodiscard]] Iterator find(const Key& key) {
        const auto found = lower_bound(key);
        return found != end() && found->first == key ? found : end();
    }
    [[nodiscard]] ConstIterator find(const Key& key) const {
        const auto found = lower_bound(key);
        return found != end() && found->first == key ? found : end();
    }
    [[nodiscard]] bool contains(const Key& key) const { return find(key) != end(); }

    [[nodiscard]] Value& at(const Key& key) {
        const auto found = find(key);
        HOG_ASSERT(found != end(), "no such key");
        return found->second;
    }
    [[nodiscard]] const Value& at(const Key& key) const {
        const auto found = find(key);
        HOG_ASSERT(found != end(), "no such key");
        return found->second;
    }

    // Adds the entry unless the key is there already, as std::map's does.
    std::pair<Iterator, bool> emplace(const Key& key, Value value) {
        const auto at = lower_bound(key);
        if (at != end() && at->first == key) {
            return {at, false};
        }
        keys_.insert(keys_.begin() + (at - begin()), key);
        return {entries_.emplace(at, key, std::move(value)), true};
    }

    // How many entries were erased: one or none.
    std::size_t erase(const Key& key) {
        const auto found = find(key);
        if (found == end()) {
            return 0;
        }
        keys_.erase(keys_.begin() + (found - begin()));
        entries_.erase(found);
        return 1;
    }

private:
    // Searches the keys alone, packed tighter than the entries, so more of them share each cache
    // line.
    [[nodiscard]] std::size_t lower_index(const Key& key) const {
        return static_cast<std::size_t>(std::ranges::lower_bound(keys_, key) - keys_.begin());
    }

    std::vector<Key> keys_; // the entries', in the same order
    std::vector<Entry> entries_;
};

} // namespace hog
