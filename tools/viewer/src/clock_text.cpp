#include <hog/viewer/clock_text.hpp>

#include <chrono>
#include <cstdint>
#include <format>

namespace hog::viewer {

std::string drift_text(GameTime game, GameTime real) {
    using std::chrono::days;
    using std::chrono::hours;
    using std::chrono::minutes;
    using std::chrono::seconds;
    const auto drift = std::chrono::floor<seconds>(game > real ? game - real : real - game);
    if (drift < seconds{5}) {
        return "on real time";
    }
    const std::string_view way = game > real ? "ahead" : "behind";
    const auto whole_days = std::chrono::floor<days>(drift);
    const auto whole_hours = std::chrono::floor<hours>(drift - whole_days);
    const auto whole_minutes = std::chrono::floor<minutes>(drift - whole_days - whole_hours);
    const auto whole_seconds = drift - whole_days - whole_hours - whole_minutes;
    // The two largest units are plenty.
    if (whole_days.count() > 0) {
        return std::format("{}d {}h {}", whole_days.count(), whole_hours.count(), way);
    }
    if (whole_hours.count() > 0) {
        return std::format("{}h {}m {}", whole_hours.count(), whole_minutes.count(), way);
    }
    if (whole_minutes.count() > 0) {
        return std::format("{}m {}s {}", whole_minutes.count(), whole_seconds.count(), way);
    }
    return std::format("{}s {}", whole_seconds.count(), way);
}

} // namespace hog::viewer
