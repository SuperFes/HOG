#pragma once

#include <hog/core/time.hpp>
#include <hog/probe/messages.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace hog::viewer {

// The world as a server last showed it: what it offers and the layers fetched from it.
class Frame {
public:
    Frame() = default;
    // Layers without a value for each of `cells` are dropped.
    Frame(probe::Welcome welcome, probe::Layers layers, std::uint32_t cells);

    [[nodiscard]] const probe::Welcome& welcome() const noexcept { return welcome_; }
    [[nodiscard]] GameTime time() const noexcept { return time_; }
    [[nodiscard]] const probe::Clock& clock() const noexcept { return clock_; }
    // One value per cell, or empty if the server didn't send it.
    [[nodiscard]] std::span<const float> layer(std::string_view name) const noexcept;

    // The species the server tracks, from its layers named "wildlife/<species>".
    [[nodiscard]] std::vector<std::string> species() const;

private:
    probe::Welcome welcome_;
    GameTime time_;
    probe::Clock clock_;
    std::vector<probe::Layer> layers_;
};

} // namespace hog::viewer
