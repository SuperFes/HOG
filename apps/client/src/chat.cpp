#include <hog/client/chat.hpp>

#include <algorithm>
#include <array>
#include <cstddef>

namespace hog::client {

namespace {

// Masked wherever they appear; no innocent word holds one.
constexpr std::array anywhere{"fuck", "shit", "bitch", "nigg", "faggot", "asshole", "bollock"};
// Masked only standing alone, clear of letters, so "class", "cockpit" and "Scunthorpe" pass.
constexpr std::array alone{"ass",  "arse",  "cunt", "dick", "cock",  "piss",    "twat",
                           "wank", "prick", "fag",  "slut", "whore", "bastard", "tits"};

[[nodiscard]] bool letter(char character) noexcept {
    return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z');
}

[[nodiscard]] char lowered(char character) noexcept {
    return character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a')
                                                : character;
}

// Whether `word` reads at `start` of `text`, whatever the case.
[[nodiscard]] bool reads(std::string_view text, std::size_t start, std::string_view word) noexcept {
    if (start + word.size() > text.size()) {
        return false;
    }
    for (std::size_t index = 0; index < word.size(); ++index) {
        if (lowered(text[start + index]) != word[index]) {
            return false;
        }
    }
    return true;
}

void mask(std::string& text, std::string_view word, bool only_alone) {
    for (std::size_t start = 0; start + word.size() <= text.size(); ++start) {
        if (!reads(text, start, word)) {
            continue;
        }
        std::size_t end = start + word.size();
        if (only_alone &&
            ((start > 0 && letter(text[start - 1])) || (end < text.size() && letter(text[end])))) {
            continue;
        }
        // The word's endings go with it, so nothing readable is left of "fucking".
        while (!only_alone && end < text.size() && letter(text[end])) {
            ++end;
        }
        std::ranges::fill(text.begin() + static_cast<std::ptrdiff_t>(start),
                          text.begin() + static_cast<std::ptrdiff_t>(end), '*');
    }
}

} // namespace

std::string censored(std::string_view text) {
    std::string masked{text};
    for (const std::string_view word : anywhere) {
        mask(masked, word, false);
    }
    for (const std::string_view word : alone) {
        mask(masked, word, true);
    }
    return masked;
}

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
