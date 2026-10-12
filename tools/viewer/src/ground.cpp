#include <hog/viewer/ground.hpp>

#include <algorithm>
#include <cmath>

namespace hog::viewer {

probe::Survey survey_of(const MapRect& shown, int width, int height, bool underground,
                        float depth) noexcept {
    // Map points run 0 to 1 from 180° west and from 90° north.
    return probe::Survey{.west = std::clamp((shown.low.x - 0.5) * 360.0, -180.0, 180.0),
                         .east = std::clamp((shown.high.x - 0.5) * 360.0, -180.0, 180.0),
                         .north = std::clamp((0.5 - shown.low.y) * 180.0, -90.0, 90.0),
                         .south = std::clamp((0.5 - shown.high.y) * 180.0, -90.0, 90.0),
                         .width = static_cast<std::uint16_t>(width),
                         .height = static_cast<std::uint16_t>(height),
                         .underground = underground,
                         .depth = depth};
}

std::optional<std::size_t> sample_at(const probe::Ground& ground, DVec2 point) noexcept {
    const probe::Survey& survey = ground.survey;
    const double longitude = (point.x - 0.5) * 360.0;
    const double latitude = (0.5 - point.y) * 180.0;
    const double across = (longitude - survey.west) / (survey.east - survey.west) * survey.width;
    const double down = (survey.north - latitude) / (survey.north - survey.south) * survey.height;
    if (!(across >= 0.0 && across < survey.width && down >= 0.0 && down < survey.height)) {
        return std::nullopt;
    }
    return (static_cast<std::size_t>(down) * survey.width) + static_cast<std::size_t>(across);
}

std::optional<probe::Survey> SurveyPacer::poll(const probe::Survey& wanted,
                                               MonotonicClock::time_point now) {
    if (wanted_ != wanted) {
        wanted_ = wanted;
        since_ = now;
    }
    if (sent_ == wanted || now - since_ < settle_) {
        return std::nullopt;
    }
    sent_ = wanted;
    return wanted;
}

} // namespace hog::viewer
