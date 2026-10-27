#include <hog/core/assert.hpp>
#include <hog/planet/drainage.hpp>
#include <hog/planet/glaciers.hpp>
#include <hog/planet/noise.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <queue>
#include <utility>
#include <vector>

namespace hog::planet {

namespace {

// Cubic metres of ice a year: less than the first cuts no trough; the second cuts one the
// reference depth deep and twice the reference half width across. Depth grows as the flow to the
// 3/8 and width as its fourth root, so the largest glaciers cut troughs a kilometre deep and
// several across.
constexpr double least_flow = 1e6;
constexpr double reference_flow = 1e7;
constexpr double reference_depth = 200.0;
constexpr double deepest = 1'200.0;
constexpr double reference_half_width = 500.0;
constexpr double widest_half = 2'500.0;
// A trough's walls rise this many times its depth over its half width, as a parabola.
constexpr double wall_rise = 1.5;
// Half widths out to which a trough is cut, should its walls not meet the ground sooner.
constexpr std::int64_t half_widths = 3;
// Ice cuts deep only where it runs fast, down steep valleys: told by how steeply it has fallen
// over some kilometres, it cuts nothing below the first slope and fully above the second, so ice
// spreading over lowland barely cuts it, while a trough keeps its depth to the sea below
// mountains.
constexpr double fall_reach = 5'000.0;
constexpr double gentlest_cut = 0.01;
constexpr double full_cut = 0.05;

struct Trough {
    std::uint32_t cell = 0;
    // Fixed-point metres: the floor under the glacier's middle, how far the walls rise over the
    // half width, and the half width.
    std::int64_t floor = 0;
    std::int64_t rise = 0;
    std::int64_t half_width = 0;
};

[[nodiscard]] std::int64_t to_fixed(double metres) {
    return std::llround(metres * static_cast<double>(fixed_one));
}

[[nodiscard]] std::int64_t multiply(std::int64_t a, std::int64_t b) {
    return (a * b) >> fixed_fraction_bits;
}

// Each cell where enough ice flows down `drainage`, upstream first, gathering each cell's ice
// into the one it flows to.
[[nodiscard]] std::vector<Trough> find_troughs(const LevelGrid& grid,
                                               std::span<const std::int64_t> heights,
                                               const Drainage& drainage, const IceBalance& balance,
                                               std::int64_t sea_level) {
    std::vector<double> flow(grid.size(), 0.0);
    // The flow into each cell times how steeply it fell, summed over what flows in.
    std::vector<double> falling(grid.size(), 0.0);
    std::vector<Trough> troughs;
    for (auto at = drainage.order.rbegin(); at != drainage.order.rend(); ++at) {
        const std::uint32_t cell = *at;
        if (heights[cell] <= sea_level) {
            continue;
        }
        const double inflow = flow[cell];
        flow[cell] = std::max(inflow + (fixed_to_double(balance(cell, heights[cell])) *
                                        static_cast<double>(grid.area(cell))),
                              0.0);
        const std::uint32_t receiver = drainage.receiver[cell];
        double fall = 0.0;
        if (receiver != cell) {
            const auto neighbors = grid.neighbors(cell);
            const auto k = static_cast<std::size_t>(std::ranges::find(neighbors, receiver) -
                                                    neighbors.begin());
            const double length = fixed_to_double(grid.distance(cell, k));
            // Into the sea, as far as its level.
            const double slope =
                std::max(fixed_to_double(heights[cell] - std::max(heights[receiver], sea_level)),
                         0.0) /
                length;
            const double before = inflow > 0.0 ? falling[cell] / inflow : slope;
            fall = ((before * fall_reach) + (slope * length)) / (fall_reach + length);
            flow[receiver] += flow[cell];
            falling[receiver] += flow[cell] * fall;
        }
        if (flow[cell] < least_flow) {
            continue;
        }
        const double fourth = std::sqrt(std::sqrt(flow[cell] / reference_flow));
        const double fast = std::clamp((fall - gentlest_cut) / (full_cut - gentlest_cut), 0.0, 1.0);
        const double depth = fast * std::min(reference_depth * fourth * std::sqrt(fourth), deepest);
        if (depth <= 0.0) {
            continue;
        }
        troughs.push_back(
            {.cell = cell,
             .floor = heights[cell] - to_fixed(depth),
             .rise = to_fixed(wall_rise * depth),
             .half_width = to_fixed(std::min(reference_half_width * fourth, widest_half))});
    }
    return troughs;
}

// Reused between troughs: how far each cell was reached from the trough's middle, and which
// were.
struct Reach {
    static constexpr std::int64_t unreached = std::numeric_limits<std::int64_t>::max();
    std::vector<std::int64_t> distance;
    std::vector<std::uint32_t> touched;
};

// Cuts the ground about a trough's middle down to its walls, spreading only through ground it
// cuts, so it never reaches over a ridge into the next valley.
void cut(const LevelGrid& grid, std::span<const std::int64_t> heights, const Trough& trough,
         Reach& reach, std::vector<std::int64_t>& carved) {
    const std::int64_t farthest = half_widths * trough.half_width;
    using Entry = std::pair<std::int64_t, std::uint32_t>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<>> queue;
    reach.distance[trough.cell] = 0;
    reach.touched.push_back(trough.cell);
    queue.emplace(0, trough.cell);
    while (!queue.empty()) {
        const auto [distance, cell] = queue.top();
        queue.pop();
        if (distance > reach.distance[cell]) {
            continue;
        }
        const std::int64_t across = (distance << fixed_fraction_bits) / trough.half_width;
        const std::int64_t depth =
            heights[cell] - (trough.floor + multiply(trough.rise, multiply(across, across)));
        if (depth <= 0) {
            continue;
        }
        carved[cell] = std::max(carved[cell], depth);
        const auto neighbors = grid.neighbors(cell);
        for (std::size_t k = 0; k < neighbors.size(); ++k) {
            const std::uint32_t next = neighbors[k];
            const std::int64_t further = distance + grid.distance(cell, k);
            if (further <= farthest && further < reach.distance[next]) {
                if (reach.distance[next] == Reach::unreached) {
                    reach.touched.push_back(next);
                }
                reach.distance[next] = further;
                queue.emplace(further, next);
            }
        }
    }
    for (const std::uint32_t cell : reach.touched) {
        reach.distance[cell] = Reach::unreached;
    }
    reach.touched.clear();
}

} // namespace

std::vector<std::int64_t> glaciate(const LevelGrid& grid, std::span<const std::int64_t> heights,
                                   const Glaciation& glaciation, std::int64_t sea_level) {
    const std::uint32_t count = grid.size();
    HOG_ASSERT(heights.size() == count && glaciation.balance, "one height per cell, and a balance");
    const Drainage drainage = drain(grid, heights, sea_level);
    std::vector<std::int64_t> carved(count, 0);
    Reach reach{.distance = std::vector<std::int64_t>(count, Reach::unreached), .touched = {}};
    for (const Trough& trough :
         find_troughs(grid, heights, drainage, glaciation.balance, sea_level)) {
        cut(grid, heights, trough, reach, carved);
    }
    return carved;
}

} // namespace hog::planet
