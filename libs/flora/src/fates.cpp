#include <hog/core/assert.hpp>
#include <hog/flora/fates.hpp>

#include <cstddef>
#include <utility>

namespace hog::flora {

namespace {

// a + t(b - a), spelled out: the ordinary rounding of each step is part of the contract.
float mixed(float a, float b, float t) noexcept {
    return a + (t * (b - a));
}

Fate mixed(const Fate& a, const Fate& b, float t) noexcept {
    return {.living = mixed(a.living, b.living, t),
            .established = mixed(a.established, b.established, t),
            .failing = mixed(a.failing, b.failing, t),
            .dead = mixed(a.dead, b.dead, t),
            .charred = mixed(a.charred, b.charred, t)};
}

// The cell a coordinate of `side` cells falls in, and how far toward the next it lies: between
// the middles of the cells either side, held at the edges.
struct Between {
    std::uint32_t low = 0;
    std::uint32_t high = 0;
    float toward = 0.0F;
};

Between between(double st, std::uint32_t side) noexcept {
    const float at = (static_cast<float>(st) * static_cast<float>(side)) - 0.5F;
    if (!(at > 0.0F)) {
        return {};
    }
    const float last = static_cast<float>(side - 1);
    if (at >= last) {
        return {.low = side - 1, .high = side - 1, .toward = 0.0F};
    }
    const auto low = static_cast<std::uint32_t>(at);
    return {.low = low, .high = low + 1, .toward = at - static_cast<float>(low)};
}

} // namespace

Fates::Fates(int level, std::vector<Fate> cells)
    : level_(level), side_(std::uint32_t{1} << static_cast<unsigned>(level)),
      cells_(std::move(cells)) {
    HOG_ASSERT(cells_.size() == std::size_t{6} * side_ * side_, "need a fate for every cell");
}

Fate Fates::at(std::uint32_t face, double s, double t) const {
    if (cells_.empty()) {
        return {};
    }
    const Between across = between(s, side_);
    const Between up = between(t, side_);
    const std::uint32_t first = face * side_ * side_;
    const auto fate = [&](std::uint32_t i, std::uint32_t j) -> const Fate& {
        return cells_[first + (j * side_) + i];
    };
    return mixed(mixed(fate(across.low, up.low), fate(across.high, up.low), across.toward),
                 mixed(fate(across.low, up.high), fate(across.high, up.high), across.toward),
                 up.toward);
}

Habitat fared(Habitat habitat, const Fate& fate) noexcept {
    habitat.living = fate.living;
    habitat.established = fate.established;
    habitat.failing = fate.failing;
    habitat.dead = fate.dead;
    habitat.charred = fate.charred;
    return habitat;
}

} // namespace hog::flora
