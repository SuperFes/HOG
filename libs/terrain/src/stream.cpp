#include "leaves.hpp"

#include <hog/planet/neighbors.hpp>
#include <hog/terrain/stream.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

namespace hog::terrain {

namespace {

// Against what's drawn beside a patch, or where nothing is, as the selection stitched it to the
// patches it culled.
std::uint8_t stitching(const std::set<planet::CellId>& drawn,
                       const std::map<planet::CellId, std::uint8_t>& selected,
                       planet::CellId cell) {
    const auto chosen = selected.find(cell);
    std::uint8_t coarser = 0;
    const auto neighbours = planet::edge_neighbors(cell);
    for (std::size_t edge = 0; edge < neighbours.size(); ++edge) {
        const planet::CellId next = neighbours.at(edge);
        const auto bit = static_cast<std::uint8_t>(1U << edge);
        if (const auto over = detail::leaf_over(drawn, next)) {
            if (over->level() < cell.level()) {
                coarser |= bit;
            }
            continue;
        }
        const auto within = drawn.lower_bound(next.range_min());
        const bool beside = within != drawn.end() && *within <= next.range_max();
        if (!beside && chosen != selected.end()) {
            coarser |= static_cast<std::uint8_t>(chosen->second & bit);
        }
    }
    return coarser;
}

} // namespace

void PatchStream::baked(Patch patch) {
    baking_.erase(patch.cell);
    heights_.insert_or_assign(patch.cell, Known{.heights = patch.heights, .baked = true});
    spread_heights(patch.cell);
    if (!held_.contains(patch.cell)) {
        waiting_.push_back(std::move(patch));
    }
}

std::optional<HeightRange> PatchStream::heights(planet::CellId cell) const {
    const auto found = heights_.find(cell);
    return found == heights_.end() ? std::nullopt : std::optional{found->second.heights};
}

void PatchStream::spread_heights(planet::CellId cell) {
    // A parent's vertices are all its children's too.
    while (!cell.is_face()) {
        const planet::CellId parent = cell.parent();
        if (const auto own = heights_.find(parent); own != heights_.end() && own->second.baked) {
            return;
        }
        HeightRange bounds{.lowest = std::numeric_limits<double>::infinity(),
                           .highest = -std::numeric_limits<double>::infinity()};
        for (int index = 0; index < 4; ++index) {
            const auto child = heights_.find(parent.child(index));
            if (child == heights_.end()) {
                return;
            }
            bounds.lowest = std::min(bounds.lowest, child->second.heights.lowest);
            bounds.highest = std::max(bounds.highest, child->second.heights.highest);
        }
        heights_.insert_or_assign(parent, Known{.heights = bounds, .baked = false});
        cell = parent;
    }
}

std::optional<planet::CellId> PatchStream::held_above(planet::CellId cell) const {
    for (int level = cell.level() - 1; level >= 0; --level) {
        if (held_.contains(cell.parent(level))) {
            return cell.parent(level);
        }
    }
    return std::nullopt;
}

bool PatchStream::covered(planet::CellId cell) const {
    if (held_above(cell)) {
        return true;
    }
    const auto within = held_.lower_bound(cell.range_min());
    return within != held_.end() && within->first <= cell.range_max();
}

std::map<planet::CellId, double> PatchStream::cover(std::span<const SelectedPatch> selected) const {
    std::map<planet::CellId, double> drawn;
    const auto add = [&](planet::CellId cell, double distance) {
        const auto [at, fresh] = drawn.try_emplace(cell, distance);
        if (!fresh) {
            at->second = std::min(at->second, distance);
        }
    };
    for (const SelectedPatch& patch : selected) {
        if (held_.contains(patch.cell)) {
            add(patch.cell, patch.distance);
        } else if (const auto above = held_above(patch.cell)) {
            add(*above, patch.distance);
        } else {
            for (auto within = held_.lower_bound(patch.cell.range_min());
                 within != held_.end() && within->first <= patch.cell.range_max(); ++within) {
                add(within->first, patch.distance);
            }
        }
    }
    // A parent standing in for some of the selection hides whatever else is held within it.
    std::vector<planet::CellId> hidden;
    for (const auto& [cell, distance] : drawn) {
        // The outermost, which nothing hides.
        for (int level = 0; level < cell.level(); ++level) {
            if (const auto above = drawn.find(cell.parent(level)); above != drawn.end()) {
                above->second = std::min(above->second, distance);
                hidden.push_back(cell);
                break;
            }
        }
    }
    for (const planet::CellId cell : hidden) {
        drawn.erase(cell);
    }
    return drawn;
}

void PatchStream::arrive(std::span<const SelectedPatch> selected, StreamFrame& frame) {
    std::map<planet::CellId, double> wanted;
    for (const SelectedPatch& patch : selected) {
        wanted.emplace(patch.cell, patch.distance);
    }
    std::erase_if(waiting_, [&](const Patch& patch) {
        return !wanted.contains(patch.cell) || held_.contains(patch.cell);
    });
    std::ranges::stable_sort(waiting_, {}, [&](const Patch& patch) {
        return std::pair{covered(patch.cell), wanted.at(patch.cell)};
    });

    // Room is made from what isn't drawn now, the least recently drawn first.
    const auto now = cover(selected);
    std::vector<planet::CellId> spare;
    for (const auto& [cell, held] : held_) {
        if (!now.contains(cell)) {
            spare.push_back(cell);
        }
    }
    std::ranges::sort(spare, std::ranges::greater{},
                      [&](planet::CellId cell) { return std::pair{held_.at(cell).drawn, cell}; });

    std::size_t taken = 0;
    for (; taken < waiting_.size() && frame.arrived.size() < settings_.uploads; ++taken) {
        if (held_.size() >= settings_.capacity) {
            if (spare.empty()) {
                break;
            }
            held_.erase(spare.back());
            frame.evicted.push_back(spare.back());
            spare.pop_back();
        }
        Patch& patch = waiting_[taken];
        held_.emplace(patch.cell, Held{.origin = patch.origin, .drawn = frame_});
        frame.arrived.push_back(std::move(patch));
    }
    waiting_.erase(waiting_.begin(), waiting_.begin() + static_cast<std::ptrdiff_t>(taken));
}

StreamFrame PatchStream::update(std::span<const SelectedPatch> selected) {
    ++frame_;
    StreamFrame frame;
    arrive(selected, frame);

    const auto drawn = cover(selected);
    std::set<planet::CellId> leaves;
    for (const auto& [cell, distance] : drawn) {
        leaves.insert(cell);
    }
    std::map<planet::CellId, std::uint8_t> selected_edges;
    for (const SelectedPatch& patch : selected) {
        selected_edges.emplace(patch.cell, patch.coarser_edges);
    }
    std::vector<std::pair<double, StreamedPatch>> nearest;
    nearest.reserve(drawn.size());
    for (const auto& [cell, distance] : drawn) {
        Held& held = held_.at(cell);
        held.drawn = frame_;
        nearest.emplace_back(
            distance, StreamedPatch{.cell = cell,
                                    .origin = held.origin,
                                    .coarser_edges = stitching(leaves, selected_edges, cell)});
    }
    std::ranges::stable_sort(nearest, {}, &std::pair<double, StreamedPatch>::first);
    frame.drawn.reserve(nearest.size());
    for (const auto& [distance, patch] : nearest) {
        frame.drawn.push_back(patch);
    }

    std::set<planet::CellId> waiting;
    for (const Patch& patch : waiting_) {
        waiting.insert(patch.cell);
    }
    std::vector<std::tuple<bool, double, planet::CellId>> missing;
    for (const SelectedPatch& patch : selected) {
        if (!held_.contains(patch.cell) && !baking_.contains(patch.cell) &&
            !waiting.contains(patch.cell)) {
            missing.emplace_back(covered(patch.cell), patch.distance, patch.cell);
        }
    }
    std::ranges::sort(missing);
    for (const auto& [covers, distance, cell] : missing) {
        if (baking_.size() >= settings_.bakes) {
            break;
        }
        baking_.insert(cell);
        frame.bakes.push_back(cell);
    }
    return frame;
}

} // namespace hog::terrain
