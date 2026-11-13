#include <hog/flow/support.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <queue>
#include <utility>

namespace hog::flow {

namespace {

// A run with the span it hangs from the ground at and the most it can.
struct Hung {
    Support::Run run;
    double span = std::numeric_limits<double>::infinity();
    double limit = 0.0;
    bool grounded = false;
};

// The box's size in columns and layers, and how thick a voxel is.
struct Box {
    int across_i = 0;
    int across_j = 0;
    int layers = 0;
    double thickness = 0.0;

    [[nodiscard]] std::size_t column(int i, int j) const {
        return (static_cast<std::size_t>(j) * static_cast<std::size_t>(across_i)) +
               static_cast<std::size_t>(i);
    }
};

using Entry = std::pair<double, std::size_t>;
using Open = std::priority_queue<Entry, std::vector<Entry>, std::greater<>>;

// The run of solid voxels up column (i, j) from `bottom`, with the span its weakest, most worn
// voxel lets it reach.
Hung measure(const Support& support, const Box& box, int i, int j, int bottom) {
    Hung hung;
    hung.run = {.i = i, .j = j, .bottom = bottom, .top = bottom};
    globe::Material weakest = support.at(i, j, bottom).solid;
    std::uint8_t worn = support.at(i, j, bottom).condition;
    for (int layer = bottom;
         layer < box.layers && support.at(i, j, layer).solid != globe::Material::Air; ++layer) {
        hung.run.top = layer;
        const Support::Cell& cell = support.at(i, j, layer);
        if (globe::properties(cell.solid).strength < globe::properties(weakest).strength) {
            weakest = cell.solid;
        }
        worn = std::min(worn, cell.condition);
    }
    const double depth = (hung.run.top - hung.run.bottom + 1) * box.thickness;
    hung.limit = support.reach(weakest, depth, worn);
    return hung;
}

// Every run in the box, lowest column first, filing each under its column in `in_column`.
std::vector<Hung> gather(const Support& support, const Box& box,
                         std::vector<std::vector<std::size_t>>& in_column) {
    std::vector<Hung> runs;
    for (int j = 0; j < box.across_j; ++j) {
        for (int i = 0; i < box.across_i; ++i) {
            const bool side = i == 0 || j == 0 || i + 1 == box.across_i || j + 1 == box.across_j;
            int layer = 0;
            while (layer < box.layers) {
                if (support.at(i, j, layer).solid == globe::Material::Air) {
                    ++layer;
                    continue;
                }
                Hung hung = measure(support, box, i, j, layer);
                layer = hung.run.top + 1;
                hung.grounded = hung.run.bottom == 0 || side;
                if (hung.grounded) {
                    hung.span = 0.0;
                }
                in_column[box.column(i, j)].push_back(runs.size());
                runs.push_back(hung);
            }
        }
    }
    return runs;
}

// Offers the runs of a column beside `run` a span through it, `step` further than its own, where
// they overlap it in height.
void offer(std::vector<Hung>& runs, const std::vector<bool>& done, Open& open,
           const std::vector<std::size_t>& column, const Support::Run& run, double span,
           double step) {
    for (const std::size_t other : column) {
        Hung& beside = runs[other];
        const bool overlaps = beside.run.bottom <= run.top && run.bottom <= beside.run.top;
        if (!overlaps || done[other] || span + step >= beside.span) {
            continue;
        }
        // Only a run that holds itself at this span carries matter further out.
        if (!beside.grounded && span + step > beside.limit) {
            continue;
        }
        beside.span = span + step;
        open.emplace(beside.span, other);
    }
}

// Spans spread out from the ground by the shortest way through runs that hold themselves.
void spread(std::vector<Hung>& runs, const std::vector<std::vector<std::size_t>>& in_column,
            const Box& box) {
    Open open;
    for (std::size_t k = 0; k < runs.size(); ++k) {
        if (runs[k].grounded) {
            open.emplace(0.0, k);
        }
    }
    std::vector<bool> done(runs.size(), false);
    while (!open.empty()) {
        const auto [span, k] = open.top();
        open.pop();
        if (done[k]) {
            continue;
        }
        done[k] = true;
        const Support::Run run = runs[k].run;
        for (int dj = -1; dj <= 1; ++dj) {
            for (int di = -1; di <= 1; ++di) {
                const int i = run.i + di;
                const int j = run.j + dj;
                if ((di == 0 && dj == 0) || i < 0 || j < 0 || i >= box.across_i ||
                    j >= box.across_j) {
                    continue;
                }
                const double step =
                    (di != 0 && dj != 0 ? std::numbers::sqrt2 : 1.0) * box.thickness;
                offer(runs, done, open, in_column[box.column(i, j)], run, span, step);
            }
        }
    }
}

} // namespace

Support::Support(int across_i, int across_j, int layers, double thickness, double gravity)
    : across_i_{across_i}, across_j_{across_j}, layers_{layers}, thickness_{thickness},
      gravity_{gravity},
      cells_(static_cast<std::size_t>(across_i) * static_cast<std::size_t>(across_j) *
             static_cast<std::size_t>(layers)) {}

double Support::reach(globe::Material material, double depth, std::uint8_t condition) const {
    const globe::Properties& matter = globe::properties(material);
    if (matter.density <= 0.0 || gravity_ <= 0.0) {
        return std::numeric_limits<double>::max();
    }
    const double strength = matter.strength * tensile * (static_cast<double>(condition) / 255.0);
    // A beam of depth t loaded by its own weight bends the root to 3 ρ g L² / t.
    return std::sqrt(strength * depth / (3.0 * matter.density * gravity_));
}

std::vector<Support::Run> Support::failing() const {
    const Box box{
        .across_i = across_i_, .across_j = across_j_, .layers = layers_, .thickness = thickness_};
    std::vector<std::vector<std::size_t>> in_column(static_cast<std::size_t>(across_i_) *
                                                    static_cast<std::size_t>(across_j_));
    std::vector<Hung> runs = gather(*this, box, in_column);
    spread(runs, in_column, box);

    std::vector<Run> failed;
    for (const Hung& hung : runs) {
        if (!hung.grounded && hung.span > hung.limit) {
            failed.push_back(hung.run);
        }
    }
    return failed;
}

} // namespace hog::flow
