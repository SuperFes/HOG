#include <hog/client/chat.hpp>

#include <algorithm>
#include <utility>

namespace hog::client {

void ChatLog::add(std::string from, std::string text, MonotonicClock::time_point now) {
    lines_.push_back({.from = std::move(from), .text = std::move(text), .at = now});
    if (lines_.size() > kept) {
        lines_.erase(lines_.begin(), lines_.end() - kept);
    }
}

std::span<const ChatLog::Line> ChatLog::fresh(MonotonicClock::time_point now) const noexcept {
    const auto first =
        std::ranges::find_if(lines_, [&](const Line& line) { return now - line.at <= fresh_for; });
    const std::span still{first, lines_.end()};
    return still.size() > most_fresh ? still.last(most_fresh) : still;
}

} // namespace hog::client
