#pragma once

#include <hog/core/assert.hpp>
#include <hog/core/handle.hpp>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>
#include <vector>

namespace hog {

// Owns values of T and hands out generational handles to them. Values live contiguously (order
// unspecified) for fast iteration; erasing moves the last value into the gap. A handle to an erased
// value never resolves again, even after its slot is reused.
template <typename T, typename Tag = T, std::unsigned_integral Generation = std::uint32_t>
class SlotMap {
public:
    using Key = Handle<Tag, Generation>;
    using IndexType = Key::IndexType;

    // Strong guarantee: if construction or allocation throws, the map is unchanged.
    template <typename... Args>
    Key emplace(Args&&... args) {
        if (free_head_ == no_slot) {
            add_free_slot();
        }
        const IndexType slot_index = free_head_;
        dense_to_slot_.push_back(slot_index);
        try {
            values_.emplace_back(std::forward<Args>(args)...);
        } catch (...) {
            dense_to_slot_.pop_back();
            throw;
        }
        Slot& slot = slots_[slot_index];
        free_head_ = slot.index;
        slot.index = static_cast<IndexType>(values_.size() - 1);
        ++slot.generation; // even (free) -> odd (occupied)
        return Key{slot_index, slot.generation};
    }

    Key insert(const T& value) { return emplace(value); }
    Key insert(T&& value) { return emplace(std::move(value)); }

    // Returns false if the key is null or stale.
    bool erase(Key key) {
        if (!contains(key)) {
            return false;
        }
        const Slot& slot = slots_[key.index()];
        const IndexType dense = slot.index;
        const auto last = static_cast<IndexType>(values_.size() - 1);
        if (dense != last) {
            values_[dense] = std::move(values_[last]);
            dense_to_slot_[dense] = dense_to_slot_[last];
            slots_[dense_to_slot_[dense]].index = dense;
        }
        values_.pop_back();
        dense_to_slot_.pop_back();
        release_slot(key.index());
        return true;
    }

    [[nodiscard]] bool contains(Key key) const noexcept {
        return !key.is_null() && key.index() < slots_.size() &&
               slots_[key.index()].generation == key.generation();
    }

    // Precondition: contains(key).
    [[nodiscard]] T& get(Key key) {
        HOG_ASSERT(contains(key), "stale or null handle");
        return values_[slots_[key.index()].index];
    }

    [[nodiscard]] const T& get(Key key) const {
        HOG_ASSERT(contains(key), "stale or null handle");
        return values_[slots_[key.index()].index];
    }

    // Invalidated by any insert or erase.
    [[nodiscard]] std::span<T> values() noexcept { return values_; }
    [[nodiscard]] std::span<const T> values() const noexcept { return values_; }

    // Calls f(key, value) for every value, in storage order. f must not insert or erase.
    template <typename F>
    void for_each(F f) {
        for (std::size_t i = 0; i < values_.size(); ++i) {
            f(key_at(i), values_[i]);
        }
    }

    template <typename F>
    void for_each(F f) const {
        for (std::size_t i = 0; i < values_.size(); ++i) {
            f(key_at(i), values_[i]);
        }
    }

    void clear() {
        for (std::size_t i = values_.size(); i > 0; --i) {
            release_slot(dense_to_slot_[i - 1]);
        }
        values_.clear();
        dense_to_slot_.clear();
    }

    void reserve(std::size_t capacity) {
        values_.reserve(capacity);
        dense_to_slot_.reserve(capacity);
        slots_.reserve(capacity);
    }

    [[nodiscard]] std::size_t size() const noexcept { return values_.size(); }
    [[nodiscard]] bool empty() const noexcept { return values_.empty(); }

private:
    static constexpr IndexType no_slot = std::numeric_limits<IndexType>::max();
    static constexpr Generation last_generation = std::numeric_limits<Generation>::max();

    // generation is odd while occupied. index is the dense position when occupied, otherwise the
    // next free slot.
    struct Slot {
        Generation generation = 0;
        IndexType index = no_slot;
    };

    [[nodiscard]] Key key_at(std::size_t dense) const noexcept {
        const IndexType slot_index = dense_to_slot_[dense];
        return Key{slot_index, slots_[slot_index].generation};
    }

    void add_free_slot() {
        HOG_ASSERT(slots_.size() < no_slot, "slot map is full");
        slots_.emplace_back();
        free_head_ = static_cast<IndexType>(slots_.size() - 1);
    }

    void release_slot(IndexType slot_index) {
        Slot& slot = slots_[slot_index];
        if (slot.generation == last_generation) {
            // Reusing it would wrap the generation and let ancient handles match again.
            slot.generation = 0;
            slot.index = no_slot;
            return;
        }
        ++slot.generation; // odd (occupied) -> even (free)
        slot.index = free_head_;
        free_head_ = slot_index;
    }

    std::vector<T> values_;
    std::vector<IndexType> dense_to_slot_;
    std::vector<Slot> slots_;
    IndexType free_head_ = no_slot;
};

} // namespace hog
