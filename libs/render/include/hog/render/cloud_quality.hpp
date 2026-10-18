#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace hog::render {

// How finely the clouds are drawn, for what the GPU can afford: how many of the picture's pixels
// each marched texel spans, and the size of the map of their shadow.
enum class CloudQuality : std::uint8_t { Low, Medium, High };

[[nodiscard]] std::string_view name_of(CloudQuality quality) noexcept;
[[nodiscard]] std::optional<CloudQuality> cloud_quality_named(std::string_view name) noexcept;
// The next finer, or the coarsest after the finest.
[[nodiscard]] CloudQuality next(CloudQuality quality) noexcept;

} // namespace hog::render
