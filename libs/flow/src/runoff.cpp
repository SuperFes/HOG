#include <hog/flow/runoff.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <queue>
#include <ranges>
#include <utility>

namespace hog::flow {
namespace {

constexpr double seconds_a_year = 365.25 * 86'400.0;

struct Waiting {
    double height = 0.0;
    std::size_t node = 0;
    std::uint64_t order = 0; // ties go first in, so a flat drains the way it was reached
};

// Lowest first, and of the same height, first in first.
struct Later {
    bool operator()(const Waiting& a, const Waiting& b) const noexcept {
        return a.height != b.height ? a.height > b.height : a.order > b.order;
    }
};

} // namespace

void Runoff::settle() {
    const std::size_t count = nodes_.size();
    surface_.resize(count);
    discharge_.assign(count, 0.0);
    standing_.assign(count, false);
    for (std::size_t n = 0; n < count; ++n) {
        surface_[n] = nodes_[n].ground;
    }
    flood();
    find_hollows();
    gather();
    channels();
}

void Runoff::flood() {
    // From the outlets inward, lowest first: each column drains to the one it was reached from,
    // and stands at least as high as the lowest way out, which in a hollow is its rim.
    const std::size_t count = nodes_.size();
    filled_.assign(count, 0.0);
    drains_.assign(count, unreached);
    order_.clear();
    order_.reserve(count);
    std::priority_queue<Waiting, std::vector<Waiting>, Later> waiting;
    std::uint64_t pushed = 0;
    const auto reach = [&](std::size_t column, double height, int to) {
        filled_[column] = std::max(height, nodes_[column].ground);
        drains_[column] = to;
        waiting.push({.height = filled_[column], .node = column, .order = pushed++});
    };
    for (std::size_t n = 0; n < count; ++n) {
        if (nodes_[n].outlet) {
            reach(n, nodes_[n].ground, none);
        }
    }
    while (order_.size() < count) {
        if (waiting.empty()) {
            // Ground no outlet reaches lets its water go at its own lowest column.
            std::size_t sink = count;
            for (std::size_t n = 0; n < count; ++n) {
                if (drains_[n] == unreached &&
                    (sink == count || nodes_[n].ground < nodes_[sink].ground)) {
                    sink = n;
                }
            }
            reach(sink, nodes_[sink].ground, none);
        }
        const Waiting next = waiting.top();
        waiting.pop();
        order_.push_back(next.node);
        for (const int beside : nodes_[next.node].next) {
            if (beside != none && drains_[static_cast<std::size_t>(beside)] == unreached) {
                reach(static_cast<std::size_t>(beside), filled_[next.node],
                      static_cast<int>(next.node));
            }
        }
    }
}

void Runoff::find_hollows() {
    // A hollow is the columns flooded over their ground to one level, joined side by side. Its
    // root is its column reached first, which drains out over the rim.
    hollow_of_.assign(nodes_.size(), none);
    hollows_.clear();
    const auto flooded = [&](std::size_t n) { return filled_[n] > nodes_[n].ground; };
    for (const std::size_t n : order_) {
        if (!flooded(n) || hollow_of_[n] != none) {
            continue;
        }
        const int index = static_cast<int>(hollows_.size());
        Hollow hollow{.root = n, .columns = {n}, .inflow = 0.0};
        hollow_of_[n] = index;
        for (std::size_t at = 0; at < hollow.columns.size(); ++at) {
            for (const int beside : nodes_[hollow.columns[at]].next) {
                if (beside == none) {
                    continue;
                }
                const auto b = static_cast<std::size_t>(beside);
                if (hollow_of_[b] == none && flooded(b) && filled_[b] == filled_[n]) {
                    hollow_of_[b] = index;
                    hollow.columns.push_back(b);
                }
            }
        }
        hollows_.push_back(std::move(hollow));
    }
}

void Runoff::gather() {
    // What runs off, gathered downhill from the highest; a hollow takes all that reaches it, and
    // once all its columns are in, lets on what its surface doesn't give back to the air.
    const double area = law_.cell * law_.cell;
    std::vector<double> gathered(nodes_.size(), 0.0);
    for (const std::size_t n : std::views::reverse(order_)) {
        const Node& node = nodes_[n];
        const bool in_hollow = hollow_of_[n] != none;
        const double rain = node.rain * area * (in_hollow ? 1.0 : 1.0 - law_.soaks);
        gathered[n] += node.inflow + std::max(rain, 0.0);
        if (!in_hollow) {
            discharge_[n] = gathered[n];
        } else {
            Hollow& hollow = hollows_[static_cast<std::size_t>(hollow_of_[n])];
            hollow.inflow += gathered[n];
            if (n != hollow.root) {
                continue;
            }
            discharge_[n] = fill(hollow);
        }
        if (drains_[n] >= 0) {
            gathered[static_cast<std::size_t>(drains_[n])] += discharge_[n];
        }
    }
}

double Runoff::fill(const Hollow& hollow) {
    const double area = law_.cell * law_.cell;
    std::vector<std::size_t> by_ground = hollow.columns;
    std::ranges::sort(by_ground, {}, [&](std::size_t c) { return nodes_[c].ground; });
    const double full_loss = law_.evaporation * area * static_cast<double>(by_ground.size());
    double level = filled_[hollow.root];
    double overflow = hollow.inflow - full_loss;
    if (overflow < 0.0) {
        // As many columns as what comes in keeps wet: the level lies over that many of the
        // lowest, part way to the next.
        const double covered = hollow.inflow / (law_.evaporation * area);
        const auto whole = static_cast<std::size_t>(covered);
        const double low = nodes_[by_ground[whole == 0 ? 0 : whole - 1]].ground;
        const double high = nodes_[by_ground[whole]].ground;
        level = low + ((high - low) * (covered - static_cast<double>(whole)));
        overflow = 0.0;
    }
    // Water stands where the hollow's lowest column reaches under the level, not in a separate
    // dip of it that the level only matches.
    const int index = hollow_of_[hollow.root];
    std::vector<std::size_t> wet{by_ground.front()};
    std::vector<bool> taken(nodes_.size(), false);
    taken[by_ground.front()] = true;
    for (std::size_t k = 0; k < wet.size(); ++k) {
        const std::size_t c = wet[k];
        if (nodes_[c].ground >= level) {
            continue;
        }
        surface_[c] = level;
        standing_[c] = true;
        for (const int beside : nodes_[c].next) {
            if (beside != none && !taken[static_cast<std::size_t>(beside)] &&
                hollow_of_[static_cast<std::size_t>(beside)] == index) {
                taken[static_cast<std::size_t>(beside)] = true;
                wet.push_back(static_cast<std::size_t>(beside));
            }
        }
    }
    return overflow;
}

void Runoff::channels() {
    // Running water stands in a channel the column wide, as deep as Manning's law takes to carry
    // it down the slope to where it drains; where it leaves the set, down the slope it came by.
    const std::size_t count = nodes_.size();
    std::vector<double> slope_in(count, 0.0);
    std::vector<double> slope_out(count, 0.0);
    for (std::size_t n = 0; n < count; ++n) {
        if (drains_[n] >= 0) {
            const auto to = static_cast<std::size_t>(drains_[n]);
            slope_out[n] = (nodes_[n].ground - nodes_[to].ground) / law_.cell;
            slope_in[to] = std::max(slope_in[to], slope_out[n]);
        }
    }
    for (std::size_t n = 0; n < count; ++n) {
        if (standing_[n] || discharge_[n] <= 0.0) {
            continue;
        }
        const double slope = std::max(drains_[n] >= 0 ? slope_out[n] : slope_in[n], 1e-4);
        const double flow = discharge_[n] / seconds_a_year;
        const double depth =
            std::pow(flow * law_.roughness / (law_.cell * std::sqrt(slope)), 3.0 / 5.0);
        if (depth >= law_.shallowest) {
            surface_[n] = nodes_[n].ground + depth;
        }
    }
}

} // namespace hog::flow
