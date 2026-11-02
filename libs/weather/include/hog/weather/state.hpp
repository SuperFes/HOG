#pragma once

#include <hog/core/result.hpp>
#include <hog/weather/field.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace hog::weather {

// Version of the bytes `encode_state` writes.
inline constexpr std::uint16_t state_version = 1;

// What of a field can't be made again from its planet: the air in every cell and the surface's
// temperature in every column.
[[nodiscard]] std::vector<std::byte> encode_state(const Field& field);

// Puts what `encode_state` wrote back into `field`, which must have the grid it was written for.
// Fails, leaving `field` as it was, on bytes cut short or damaged, or from another grid.
[[nodiscard]] Result<void> decode_state(Field& field, std::span<const std::byte> bytes);

} // namespace hog::weather
