#include <hog/flow/support.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <queue>
#include <utility>

namespace hog::flow {

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
    struct Hung {
        Run run;
        double span = std::numeric_limits<double>::infinity();
        double limit = 0.0;
        bool grounded = false;
    };
    std::vector<Hung> runs;
    // Runs by column, for finding those beside one.
    std::vector<std::vector<std::size_t>> in_column(static_cast<std::size_t>(across_i_) *
                                                    static_cast<std::size_t>(across_j_));
    for (int j = 0; j < across_j_; ++j) {
        for (int i = 0; i < across_i_; ++i) {
            const bool side = i == 0 || j == 0 || i + 1 == across_i_ || j + 1 == across_j_;
            int layer = 0;
            while (layer < layers_) {
                if (at(i, j, layer).solid == globe::Material::Air) {
                    ++layer;
                    continue;
                }
                Hung hung;
                hung.run = {.i = i, .j = j, .bottom = layer, .top = layer};
                globe::Material weakest = at(i, j, layer).solid;
                std::uint8_t worn = at(i, j, layer).condition;
                for (; layer < layers_ && at(i, j, layer).solid != globe::Material::Air; ++layer) {
                    hung.run.top = layer;
                    const Cell& cell = at(i, j, layer);
                    if (globe::properties(cell.solid).strength < globe::properties(weakest).strength) {
                        weakest = cell.solid;
                    }
                    worn = std::min(worn, cell.condition);
                }
                const double depth = (hung.run.top - hung.run.bottom + 1) * thickness_;
                hung.limit = reach(weakest, depth, worn);
                hung.grounded = hung.run.bottom == 0 || side;
                if (hung.grounded) {
                    hung.span = 0.0;
                }
                in_column[(static_cast<std::size_t>(j) * static_cast<std::size_t>(across_i_)) +
                          static_cast<std::size_t>(i)]
                    .push_back(runs.size());
                runs.push_back(hung);
            }
        }
    }

    // Spans spread out from the ground by the shortest way through runs that hold themselves.
    using Entry = std::pair<double, std::size_t>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<>> open;
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
        const Run run = runs[k].run;
        for (int dj = -1; dj <= 1; ++dj) {
            for (int di = -1; di <= 1; ++di) {
                const int i = run.i + di;
                const int j = run.j + dj;
                if ((di == 0 && dj == 0) || i < 0 || j < 0 || i >= across_i_ || j >= across_j_) {
                    continue;
                }
                const double step = (di != 0 && dj != 0 ? std::numbers::sqrt2 : 1.0) * thickness_;
                for (const std::size_t other :
                     in_column[(static_cast<std::size_t>(j) * static_cast<std::size_t>(across_i_)) +
                               static_cast<std::size_t>(i)]) {
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
        }
    }

    std::vector<Run> failed;
    for (const Hung& hung : runs) {
        if (!hung.grounded && hung.span > hung.limit) {
            failed.push_back(hung.run);
        }
    }
    return failed;
}

} // namespace hog::flow
