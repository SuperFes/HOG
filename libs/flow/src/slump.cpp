#include <hog/flow/slump.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace hog::flow {
namespace {

struct Side {
    int di;
    int dj;
    bool diagonal;
};

constexpr std::array<Side, 8> sides{{{1, 0, false},
                                     {-1, 0, false},
                                     {0, 1, false},
                                     {0, -1, false},
                                     {1, 1, true},
                                     {1, -1, true},
                                     {-1, 1, true},
                                     {-1, -1, true}}};

// Each side asks for a fraction of the excess, so all eight together can't overshoot level.
constexpr std::int64_t share = 16;

} // namespace

Slump::Slump(int width, int depth)
    : width_{width}, depth_{depth}, ground_(static_cast<std::size_t>(width * depth), 0),
      grain_(static_cast<std::size_t>(width * depth), 0) {}

std::int64_t Slump::total() const {
    std::int64_t sum = 0;
    for (const std::int64_t amount : grain_) {
        sum += amount;
    }
    return sum;
}

std::int64_t Slump::step(std::int64_t rise) {
    const auto diagonal_rise = static_cast<std::int64_t>(std::llround(static_cast<double>(rise) * std::numbers::sqrt2));
    std::vector<std::int64_t> change(grain_.size(), 0);

    for (int j = 0; j < depth_; ++j) {
        for (int i = 0; i < width_; ++i) {
            const std::int64_t available = grain(i, j);
            if (available == 0) {
                continue;
            }
            std::array<std::int64_t, 8> asks{};
            std::int64_t asked = 0;
            for (std::size_t s = 0; s < sides.size(); ++s) {
                const int ni = i + sides[s].di;
                const int nj = j + sides[s].dj;
                if (ni < 0 || nj < 0 || ni >= width_ || nj >= depth_) {
                    continue;
                }
                const std::int64_t held = sides[s].diagonal ? diagonal_rise : rise;
                const std::int64_t excess = surface(i, j) - surface(ni, nj) - held;
                if (excess > 0) {
                    asks[s] = excess / (sides[s].diagonal ? 2 * share : share);
                    asked += asks[s];
                }
            }
            if (asked == 0) {
                continue;
            }
            // Never more than the column has to give.
            const std::int64_t scaled = std::min(asked, available);
            for (std::size_t s = 0; s < sides.size(); ++s) {
                if (asks[s] == 0) {
                    continue;
                }
                const std::int64_t moved = asks[s] * scaled / asked;
                change[index(i, j)] -= moved;
                change[index(i + sides[s].di, j + sides[s].dj)] += moved;
            }
        }
    }

    std::int64_t moved = 0;
    for (std::size_t k = 0; k < grain_.size(); ++k) {
        grain_[k] += change[k];
        moved += std::max<std::int64_t>(change[k], 0);
    }
    return moved;
}

} // namespace hog::flow
