#pragma once

#include <hog/core/math.hpp>
#include <hog/core/time.hpp>
#include <hog/probe/messages.hpp>
#include <hog/viewer/map_view.hpp>

#include <chrono>
#include <cstddef>
#include <optional>

namespace hog::viewer {

// The survey of the shown part of the map with a sample under each pixel of a width by height
// image of it.
[[nodiscard]] probe::Survey survey_of(const MapRect& shown, int width, int height, bool underground,
                                      float depth) noexcept;

// The sample covering a map point, or none off the surveyed part.
[[nodiscard]] std::optional<std::size_t> sample_at(const probe::Ground& ground,
                                                   DVec2 point) noexcept;

// Holds a survey back until what's wanted has stayed the same a moment, so zooming and panning
// don't send one every frame.
class SurveyPacer {
public:
    explicit SurveyPacer(std::chrono::milliseconds settle) noexcept : settle_(settle) {}

    // The survey to send now, if any: what's wanted, once it has gone unchanged for the settle
    // time, unless it was the last one sent.
    [[nodiscard]] std::optional<probe::Survey> poll(const probe::Survey& wanted,
                                                    MonotonicClock::time_point now);
    // Forgets what was sent, so it's sent again, as to a server just connected.
    void forget() noexcept { sent_.reset(); }

private:
    std::chrono::milliseconds settle_;
    std::optional<probe::Survey> wanted_;
    MonotonicClock::time_point since_;
    std::optional<probe::Survey> sent_;
};

} // namespace hog::viewer
