#pragma once

#include <compare>
#include <concepts>
#include <cstdint>

namespace hog {

template <typename T, typename Tag, std::unsigned_integral Generation>
class SlotMap;

// A weak reference into a SlotMap: an index plus the generation the slot had when the handle was
// issued. Tag keeps handles to different kinds of objects from being mixed up.
template <typename Tag, std::unsigned_integral Generation = std::uint32_t>
class Handle {
public:
    using IndexType = std::uint32_t;
    using GenerationType = Generation;

    constexpr Handle() noexcept = default;

    [[nodiscard]] constexpr bool is_null() const noexcept { return generation_ == 0; }
    [[nodiscard]] constexpr IndexType index() const noexcept { return index_; }
    [[nodiscard]] constexpr Generation generation() const noexcept { return generation_; }

    constexpr auto operator<=>(const Handle&) const noexcept = default;

private:
    template <typename, typename, std::unsigned_integral>
    friend class SlotMap;

    constexpr Handle(IndexType index, Generation generation) noexcept
        : index_(index), generation_(generation) {}

    IndexType index_ = 0;
    Generation generation_ = 0;
};

} // namespace hog
