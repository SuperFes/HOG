#include <hog/core/stage_timer.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <format>
#include <iterator>
#include <string>
#include <string_view>

namespace hog {

void StageTimer::add(std::string_view name, std::chrono::nanoseconds took) {
    const auto found = std::ranges::find(stages_, name, &Stage::name);
    if (found == stages_.end()) {
        stages_.push_back({.name = std::string{name}, .took = took, .runs = 1});
        return;
    }
    found->took += took;
    ++found->runs;
}

std::string StageTimer::report() const {
    std::size_t widest = std::string_view{"total"}.size();
    std::chrono::nanoseconds total{};
    for (const Stage& stage : stages_) {
        widest = std::max(widest, stage.name.size());
        total += stage.took;
    }
    const auto seconds = [](std::chrono::nanoseconds took) {
        return std::chrono::duration<double>(took).count();
    };
    std::string out;
    for (const Stage& stage : stages_) {
        std::format_to(std::back_inserter(out), "{:<{}}  {:8.3f} s", stage.name, widest,
                       seconds(stage.took));
        if (stage.runs > 1) {
            std::format_to(std::back_inserter(out), "  ({} runs)", stage.runs);
        }
        out += '\n';
    }
    std::format_to(std::back_inserter(out), "{:<{}}  {:8.3f} s\n", "total", widest, seconds(total));
    return out;
}

} // namespace hog
