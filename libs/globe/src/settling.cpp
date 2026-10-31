#include <hog/core/assert.hpp>
#include <hog/globe/settling.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

namespace hog::globe {

double strength_now(const Hexel& hexel) {
    const Properties& material = properties(hexel.material);
    // Held over the ages, rock keeps its strength to half its melting point, and creeps freely
    // past four fifths of it.
    const double weakened =
        material.melts > 0.0 ? ((0.8 * material.melts) - hexel.temperature) / (0.3 * material.melts)
                             : 0.0;
    return material.strength * std::clamp(weakened, 0.0, 1.0);
}

double bearable(const Hexel& hexel, double gravity) {
    if (gravity <= 0.0) {
        return std::numeric_limits<double>::infinity();
    }
    return strength_now(hexel) / (properties(hexel.material).density * gravity);
}

int top_of(std::span<const Hexel> column) {
    for (auto shell = static_cast<int>(column.size()); shell > 0; --shell) {
        if (column[static_cast<std::size_t>(shell - 1)].material != Material::Air) {
            return shell;
        }
    }
    return 0;
}

Settler::Settler(Globe& globe, const HexGrid& grid, Gravity gravity)
    : globe_(&globe), grid_(&grid), gravity_(gravity), tops_(grid.size(), 0), seen_(grid.size(), 0),
      queued_(grid.size(), false), step_(std::atan(2.0) / std::ldexp(1.0, globe.level())) {
    HOG_ASSERT(grid.level() == globe.level() && grid.size() == globe.size(),
               "the grid is not the globe's");
    for (std::uint32_t column = 0; column < grid.size(); ++column) {
        tops_[column] = top_of(globe.column_at(column));
    }
}

std::optional<Settler::Fall> Settler::fall_to(std::uint32_t column) {
    const Shells& shells = globe_->shells();
    const int top = tops_[column];
    const auto hexels = globe_->column_at(column);
    ++mark_;
    seen_[column] = mark_;
    ring_.assign(1, {.column = column, .top = top});
    for (int reach = 1; reach <= farthest && !ring_.empty(); ++reach) {
        beyond_.clear();
        // Out through columns open at the falling hexel's shell.
        for (const Fall& from : ring_) {
            for (const std::uint32_t next : grid_->neighbors(from.column)) {
                if (seen_[next] == mark_) {
                    continue;
                }
                seen_[next] = mark_;
                if (tops_[next] < top) {
                    beyond_.push_back({.column = next, .top = tops_[next]});
                }
            }
        }
        std::swap(ring_, beyond_);
        const auto lowest = std::ranges::min_element(ring_, {}, &Fall::top);
        if (lowest == ring_.end() || lowest->top > top - 2) {
            continue;
        }
        // Culmann's wedge: a slope as steep as from here to there stands up to four times as high
        // as a sheer face would, and higher the gentler it is.
        const double high = shells.floor(top);
        const double low = shells.floor(lowest->top);
        const double drop = high - low;
        const double across = reach * step_ * high;
        const double pull = gravity_.at(0.5 * (high + low));
        double stands = std::numeric_limits<double>::infinity();
        for (int shell = lowest->top; shell < top; ++shell) {
            stands = std::min(stands, bearable(hexels[static_cast<std::size_t>(shell)], pull));
        }
        const double slope = std::atan2(drop, across);
        if (drop > 4.0 * stands / std::tan(slope / 2.0)) {
            return *lowest;
        }
    }
    return std::nullopt;
}

void Settler::queue(std::uint32_t column) {
    if (!queued_[column]) {
        queued_[column] = true;
        pending_.push_back(column);
    }
}

std::uint64_t Settler::settle(std::span<const HexCell> unsettled) {
    for (const HexCell cell : unsettled) {
        const auto column = static_cast<std::uint32_t>(globe_->index_of(cell));
        tops_[column] = top_of(globe_->column_at(column));
    }
    // What changed may fall, and so may what stood against it.
    for (const HexCell cell : unsettled) {
        const auto column = static_cast<std::uint32_t>(globe_->index_of(cell));
        queue(column);
        for (const std::uint32_t next : grid_->neighbors(column)) {
            queue(next);
        }
    }
    std::uint64_t moved = 0;
    while (!pending_.empty()) {
        const std::uint32_t column = pending_.back();
        pending_.pop_back();
        queued_[column] = false;
        if (tops_[column] == 0) {
            continue;
        }
        const auto fall = fall_to(column);
        if (!fall) {
            continue;
        }
        Hexel& falling = globe_->column_at(column)[static_cast<std::size_t>(tops_[column] - 1)];
        globe_->column_at(fall->column)[static_cast<std::size_t>(fall->top)] = falling;
        falling = Hexel{};
        --tops_[column];
        ++tops_[fall->column];
        ++moved;
        queue(column);
        queue(fall->column);
        for (const std::uint32_t next : grid_->neighbors(column)) {
            queue(next);
        }
    }
    return moved;
}

std::uint64_t settle(Globe& globe, std::span<const HexCell> unsettled, const Gravity& gravity) {
    const HexGrid grid{globe};
    return Settler{globe, grid, gravity}.settle(unsettled);
}

} // namespace hog::globe
