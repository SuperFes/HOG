#include "wildlife_stages.hpp"

namespace hog::sim::detail {

std::array<float, 4> leaving(const LifeContext& step, std::span<const float> appeal,
                             std::span<const float> scent, std::uint32_t species,
                             std::uint32_t cell) {
    const Species& one = step.config->ecology.species[species];
    std::array<float, 4> leaving{};
    if (dormant(one, static_cast<double>(step.conditions.temperature[cell]))) {
        return leaving;
    }
    const auto& next = step.grid->neighbors(cell);
    const auto here = static_cast<double>(scent[cell]);
    // Animals stray only onto ground they can live on, but cross any ground they can pass over
    // towards better.
    std::array<bool, 4> open{};
    std::array<bool, 4> livable{};
    std::array<double, 4> better{};
    double most = 0.0;
    double sum = 0.0;
    int ways = 0;
    for (std::size_t k = 0; k < 4; ++k) {
        open[k] = passable(one, step.habitat[next[k]]);
        livable[k] =
            open[k] && one.habitat.at(static_cast<std::size_t>(step.habitat[next[k]].biome)) > 0.0;
        const auto there = static_cast<double>(scent[next[k]]);
        if (open[k] && there > here) {
            better[k] = (there - here) / there;
            most = std::max(most, better[k]);
            sum += better[k];
        }
        ways += livable[k] ? 1 : 0;
    }
    // Shares of the cell crossed in a step. Animals head off in earnest once better ground is as
    // much better as the scent fades over a cell.
    const double width = std::sqrt(step.areas[cell]);
    const double wander = ways > 0 ? std::min(one.wander * step.days / width, 0.5) / ways : 0.0;
    const double urgency = std::min(most / (1.0 - step.config->scent_fade), 1.0);
    const double reach = std::min(one.speed * step.days / width, 1.0);
    const double head = sum > 0.0 ? reach * urgency / sum : 0.0;
    std::array<double, 4> share{};
    double total = 0.0;
    for (std::size_t k = 0; k < 4; ++k) {
        double heading = head * better[k];
        // Better ground right next door gets worse as animals arrive, so only enough go to close
        // part of the gap; any more and they'd all turn round and come back the next step.
        if (scent[next[k]] <= appeal[next[k]]) {
            constexpr double closing = 0.5;
            heading = std::min(heading, better[k] * closing);
        }
        share[k] = (livable[k] ? wander : 0.0) + heading;
        total += share[k];
    }
    constexpr double most_leaving = 0.95;
    const double scale = total > most_leaving ? most_leaving / total : 1.0;
    for (std::size_t k = 0; k < 4; ++k) {
        leaving[k] = static_cast<float>(share[k] * scale);
    }
    return leaving;
}

} // namespace hog::sim::detail
