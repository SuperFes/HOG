#include "leaves.hpp"

#include <hog/core/profile.hpp>
#include <hog/planet/neighbors.hpp>
#include <hog/terrain/stream.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <numbers>
#include <optional>
#include <set>
#include <span>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace hog::terrain {

namespace {

// Against what's drawn beside a patch, or where nothing is, as the selection stitched it to the
// patches it culled. Both sorted by cell.
std::uint8_t stitching(detail::Leaves drawn,
                       std::span<const std::pair<planet::CellId, std::uint8_t>> selected,
                       planet::CellId cell) {
    const auto chosen = std::ranges::lower_bound(selected, cell, {},
                                                 &std::pair<planet::CellId, std::uint8_t>::first);
    const bool was_chosen = chosen != selected.end() && chosen->first == cell;
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
        if (!detail::any_within(drawn, next) && was_chosen) {
            coarser |= static_cast<std::uint8_t>(chosen->second & bit);
        }
    }
    return coarser;
}

// The cells among those tracked that a change reaches: the one it lies in at each coarser level
// and those beside that, then those within it, and those beside it touching it.
template <typename Cells>
std::vector<planet::CellId> reached(const Cells& cells, planet::CellId change) {
    const auto cell_of = [](const auto& entry) -> planet::CellId {
        if constexpr (std::is_same_v<std::decay_t<decltype(entry)>, planet::CellId>) {
            return entry;
        } else {
            return entry.first;
        }
    };
    std::vector<planet::CellId> found;
    const auto add_tracked = [&](planet::CellId cell) {
        if (cells.contains(cell)) {
            found.push_back(cell);
        }
    };
    for (int level = 0; level < change.level(); ++level) {
        const planet::CellId above = change.parent(level);
        add_tracked(above);
        const auto beside = planet::vertex_neighbors(above);
        for (const planet::CellId next : beside.cells()) {
            add_tracked(next);
        }
    }
    const auto within = [&](planet::CellId area, auto keep) {
        for (auto at = cells.lower_bound(area.range_min());
             at != cells.end() && cell_of(*at) <= area.range_max(); ++at) {
            if (keep(cell_of(*at))) {
                found.push_back(cell_of(*at));
            }
        }
    };
    within(change, [](planet::CellId /*cell*/) { return true; });
    const auto beside = planet::vertex_neighbors(change);
    for (const planet::CellId next : beside.cells()) {
        within(next, [&](planet::CellId cell) { return reaches(cell, change); });
    }
    return found;
}

// Roughly, metres between a patch's vertices: a face spans a quarter turn.
double spacing_of(const Patch& patch) {
    return glm::length(patch.origin.metres()) * (std::numbers::pi / 2.0) /
           static_cast<double>(std::int64_t{1} << (patch.cell.level() + patch_depth));
}

} // namespace

void PatchStream::baked(Patch patch) {
    settled_ = false;
    baking_.erase(patch.cell);
    if (outdated_.erase(patch.cell) != 0) {
        return;
    }
    heights_.insert_or_assign(
        patch.cell, Known{.heights = patch.heights, .spacing = spacing_of(patch), .baked = true});
    spread_heights(patch.cell);
    if (!fresh(patch.cell)) {
        waiting_.push_back(std::move(patch));
    }
}

void PatchStream::changed(planet::CellId cell) {
    settled_ = false;
    for (const planet::CellId held : reached(held_, cell)) {
        held_.at(held).stale = true;
    }
    for (const planet::CellId baking : reached(baking_, cell)) {
        outdated_.insert(baking);
    }
    // Bounds built on what's reached are reached too, lying over it.
    for (const planet::CellId known : reached(heights_, cell)) {
        heights_.erase(known);
    }
    std::erase_if(waiting_, [&](const Patch& patch) { return reaches(patch.cell, cell); });
}

std::optional<HeightRange> PatchStream::heights(planet::CellId cell) const {
    const auto found = heights_.find(cell);
    return found == heights_.end() ? std::nullopt : std::optional{found->second.heights};
}

std::optional<HeightRange> PatchStream::bounds(planet::CellId cell) const {
    for (int level = cell.level(); level >= 0; --level) {
        const auto found = heights_.find(cell.parent(level));
        if (found == heights_.end()) {
            continue;
        }
        if (level == cell.level()) {
            return found->second.heights;
        }
        // Finer ground can rise and fall between the ancestor's vertices.
        const double margin = settings_.margin * found->second.spacing;
        return HeightRange{.lowest = found->second.heights.lowest - margin,
                           .highest = found->second.heights.highest + margin};
    }
    return std::nullopt;
}

planet::CellId PatchStream::to_bake(planet::CellId cell) const {
    if (settings_.coarse_steps <= 0) {
        return cell;
    }
    int known = -1;
    for (int level = cell.level(); level >= 0; --level) {
        if (heights_.contains(cell.parent(level))) {
            known = level;
            break;
        }
    }
    if (cell.level() - known <= settings_.coarse_steps) {
        return cell;
    }
    return cell.parent(known + settings_.coarse_steps);
}

void PatchStream::spread_heights(planet::CellId cell) {
    // A parent's vertices are all its children's too.
    while (!cell.is_face()) {
        const planet::CellId parent = cell.parent();
        if (const auto own = heights_.find(parent); own != heights_.end() && own->second.baked) {
            return;
        }
        Known bounds{.heights = {.lowest = std::numeric_limits<double>::infinity(),
                                 .highest = -std::numeric_limits<double>::infinity()},
                     .spacing = 0.0,
                     .baked = false};
        for (int index = 0; index < 4; ++index) {
            const auto child = heights_.find(parent.child(index));
            if (child == heights_.end()) {
                return;
            }
            bounds.heights.lowest = std::min(bounds.heights.lowest, child->second.heights.lowest);
            bounds.heights.highest =
                std::max(bounds.heights.highest, child->second.heights.highest);
            bounds.spacing = std::max(bounds.spacing, child->second.spacing);
        }
        heights_.insert_or_assign(parent, bounds);
        cell = parent;
    }
}

bool PatchStream::fresh(planet::CellId cell) const {
    const auto held = held_.find(cell);
    return held != held_.end() && !held->second.stale;
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

void PatchStream::stand_in(const std::map<planet::CellId, double>& standing,
                           std::map<planet::CellId, double>& drawn) const {
    // An ancestor stands in for missing ground only if it already did, or if nothing finer is
    // drawn beneath it: ground drawn finely never turns coarse again for a patch or two that
    // aren't baked yet, which are left out until they are. Finest first, so a coarser one
    // doesn't hide them.
    std::vector<std::pair<planet::CellId, double>> by_level(standing.begin(), standing.end());
    std::ranges::stable_sort(by_level, std::ranges::greater{},
                             [](const auto& entry) { return entry.first.level(); });
    for (const auto& [cell, distance] : by_level) {
        const bool stood_in = held_.at(cell).drawn + 1 == frame_;
        const auto within = drawn.lower_bound(cell.range_min());
        const bool finer =
            within != drawn.end() && within->first <= cell.range_max() && within->first != cell;
        if (stood_in || !finer) {
            const auto [at, added] = drawn.try_emplace(cell, distance);
            if (!added) {
                at->second = std::min(at->second, distance);
            }
        }
    }
}

std::map<planet::CellId, double> PatchStream::cover(std::span<const SelectedPatch> selected) const {
    HOG_ZONE("cover patches");
    std::map<planet::CellId, double> drawn;
    const auto add = [&](planet::CellId cell, double distance) {
        const auto [at, fresh] = drawn.try_emplace(cell, distance);
        if (!fresh) {
            at->second = std::min(at->second, distance);
        }
    };
    std::map<planet::CellId, double> standing;
    for (const SelectedPatch& patch : selected) {
        if (held_.contains(patch.cell)) {
            add(patch.cell, patch.distance);
        } else if (const auto above = held_above(patch.cell)) {
            const auto [at, fresh] = standing.try_emplace(*above, patch.distance);
            if (!fresh) {
                at->second = std::min(at->second, patch.distance);
            }
        } else {
            for (auto within = held_.lower_bound(patch.cell.range_min());
                 within != held_.end() && within->first <= patch.cell.range_max(); ++within) {
                add(within->first, patch.distance);
            }
        }
    }
    stand_in(standing, drawn);
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

std::map<planet::CellId, double>
PatchStream::wanted_waiting(std::span<const SelectedPatch> selected) const {
    std::map<planet::CellId, double> selected_at;
    for (const SelectedPatch& patch : selected) {
        selected_at.emplace(patch.cell, patch.distance);
    }
    std::map<planet::CellId, double> wanted;
    for (const Patch& patch : waiting_) {
        if (fresh(patch.cell)) {
            continue;
        }
        if (const auto chosen = selected_at.find(patch.cell); chosen != selected_at.end()) {
            wanted.emplace(patch.cell, chosen->second);
            continue;
        }
        for (auto within = selected_at.lower_bound(patch.cell.range_min());
             within != selected_at.end() && within->first <= patch.cell.range_max(); ++within) {
            const auto [at, added] = wanted.try_emplace(patch.cell, within->second);
            if (!added) {
                at->second = std::min(at->second, within->second);
            }
        }
    }
    return wanted;
}

void PatchStream::arrive(std::span<const SelectedPatch> selected, StreamFrame& frame) {
    HOG_ZONE("arrive patches");
    if (waiting_.empty()) {
        return;
    }
    const std::map<planet::CellId, double> wanted = wanted_waiting(selected);
    std::erase_if(waiting_, [&](const Patch& patch) { return !wanted.contains(patch.cell); });
    std::ranges::stable_sort(waiting_, {}, [&](const Patch& patch) {
        return std::pair{covered(patch.cell), wanted.at(patch.cell)};
    });

    // Room is made from what isn't drawn now, the least recently drawn first, if what arrives
    // can run out of it.
    const std::size_t arriving = std::min<std::size_t>(waiting_.size(), settings_.uploads);
    const auto unheld = std::ranges::count_if(
        waiting_.begin(), waiting_.begin() + static_cast<std::ptrdiff_t>(arriving),
        [&](const Patch& patch) { return !held_.contains(patch.cell); });
    std::vector<planet::CellId> spare;
    if (held_.size() + static_cast<std::size_t>(unheld) > settings_.capacity) {
        const auto now = cover(selected);
        std::vector<std::pair<std::uint64_t, planet::CellId>> by_drawn;
        for (const auto& [cell, held] : held_) {
            if (!now.contains(cell)) {
                by_drawn.emplace_back(held.drawn, cell);
            }
        }
        std::ranges::sort(by_drawn, std::ranges::greater{});
        spare.reserve(by_drawn.size());
        for (const auto& [drawn, cell] : by_drawn) {
            spare.push_back(cell);
        }
    }

    std::size_t taken = 0;
    for (; taken < waiting_.size() && frame.arrived.size() < settings_.uploads; ++taken) {
        Patch& patch = waiting_[taken];
        if (const auto held = held_.find(patch.cell); held != held_.end()) {
            held->second.stale = false;
        } else {
            if (held_.size() >= settings_.capacity) {
                if (spare.empty()) {
                    break;
                }
                held_.erase(spare.back());
                frame.evicted.push_back(spare.back());
                spare.pop_back();
            }
            held_.emplace(patch.cell, Held{.origin = patch.origin, .drawn = frame_});
        }
        frame.arrived.push_back(std::move(patch));
    }
    waiting_.erase(waiting_.begin(), waiting_.begin() + static_cast<std::ptrdiff_t>(taken));
}

StreamFrame PatchStream::update(std::span<const SelectedPatch> selected) {
    HOG_ZONE("stream patches");
    // Settled on the same selection, it would draw just the same again: what stood in still would,
    // and the patches' drawn stamps keep their order.
    if (settled_ && std::ranges::equal(selected, settled_on_)) {
        StreamFrame same;
        same.drawn = settled_drawn_;
        return same;
    }
    ++frame_;
    StreamFrame frame;
    arrive(selected, frame);

    std::vector<SelectedPatch> in_view;
    std::ranges::copy_if(selected, std::back_inserter(in_view), [](const SelectedPatch& patch) {
        return patch.in_view && !patch.under_sea;
    });
    const auto drawn = cover(in_view);
    std::vector<planet::CellId> leaves;
    leaves.reserve(drawn.size());
    for (const auto& [cell, distance] : drawn) {
        leaves.push_back(cell);
    }
    std::vector<std::pair<planet::CellId, std::uint8_t>> selected_edges;
    selected_edges.reserve(in_view.size());
    for (const SelectedPatch& patch : in_view) {
        selected_edges.emplace_back(patch.cell, patch.coarser_edges);
    }
    std::ranges::sort(selected_edges);
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
    std::set<planet::CellId> asked;
    for (const SelectedPatch& patch : selected) {
        // Held fresh, its heights are known, so it's its own bake, and needs none.
        if (fresh(patch.cell)) {
            continue;
        }
        const planet::CellId cell = to_bake(patch.cell);
        if (!fresh(cell) && !baking_.contains(cell) && !waiting.contains(cell) &&
            asked.insert(cell).second) {
            missing.emplace_back(covered(cell), patch.distance, cell);
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
    settled_ = frame.bakes.empty() && frame.arrived.empty() && frame.evicted.empty() &&
               baking_.empty() && waiting_.empty();
    if (settled_) {
        settled_on_.assign(selected.begin(), selected.end());
        settled_drawn_ = frame.drawn;
    }
    return frame;
}

} // namespace hog::terrain
