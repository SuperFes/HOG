#include <hog/flow/slump.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace hog::flow {
namespace {

// Each side asks for a fraction of the excess, so all eight together can't overshoot level.
constexpr std::int64_t share = 16;

struct Ask {
    std::size_t from;
    std::size_t to;
    std::size_t side;
    std::int64_t amount;
    bool falls;
};

// Asks each lower side of column `n` for its share of the excess over the rise the column holds.
void ask_sides(const Slump& slump, std::size_t n, std::vector<Ask>& asks) {
    const Slump::Node& node = slump[n];
    if (node.grain == 0) {
        return;
    }
    const std::size_t first = asks.size();
    std::int64_t asked = 0;
    for (std::size_t s = 0; s < Slump::sides; ++s) {
        if (node.next[s] == Slump::none) {
            continue;
        }
        const bool diagonal = s >= 4;
        const auto held = diagonal
                              ? std::llround(static_cast<double>(node.rise) * std::numbers::sqrt2)
                              : node.rise;
        const auto to = static_cast<std::size_t>(node.next[s]);
        const std::int64_t excess = slump.surface(n) - slump.surface(to) - held;
        const std::int64_t ask = excess > 0 ? excess / (diagonal ? 2 * share : share) : 0;
        if (ask > 0) {
            asks.push_back({.from = n,
                            .to = to,
                            .side = s,
                            .amount = ask,
                            .falls = slump.surface(n) - slump.surface(to) > node.reach});
            asked += ask;
        }
    }
    // Never more than the column has to give.
    if (asked > node.grain) {
        for (std::size_t k = first; k < asks.size(); ++k) {
            asks[k].amount = asks[k].amount * node.grain / asked;
        }
    }
}

} // namespace

std::int64_t Slump::total() const {
    std::int64_t sum = 0;
    for (const Node& node : nodes_) {
        sum += node.grain;
    }
    return sum;
}

std::int64_t Slump::step() {
    std::vector<Ask> asks;
    for (std::size_t n = 0; n < nodes_.size(); ++n) {
        ask_sides(*this, n, asks);
    }

    // And never more than a column has room for: its inflows share what's left.
    std::vector<std::int64_t> coming(nodes_.size(), 0);
    for (const Ask& ask : asks) {
        if (!ask.falls) {
            coming[ask.to] += ask.amount;
        }
    }
    std::int64_t moved = 0;
    std::vector<std::int64_t> change(nodes_.size(), 0);
    for (const Ask& ask : asks) {
        if (ask.falls) {
            change[ask.from] -= ask.amount;
            falls_.push_back({.from = ask.from, .side = ask.side, .amount = ask.amount});
            moved += ask.amount;
            continue;
        }
        const Node& to = nodes_[ask.to];
        const std::int64_t left = std::max<std::int64_t>(to.room - to.grain, 0);
        const std::int64_t amount =
            coming[ask.to] > left ? ask.amount * left / coming[ask.to] : ask.amount;
        change[ask.from] -= amount;
        change[ask.to] += amount;
        moved += amount;
    }
    for (std::size_t n = 0; n < nodes_.size(); ++n) {
        nodes_[n].grain += change[n];
    }
    return moved;
}

} // namespace hog::flow
