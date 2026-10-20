#include <hog/planet/world.hpp>

#include <algorithm>
#include <format>
#include <tuple>
#include <type_traits>
#include <utility>

namespace hog::planet {

namespace {

[[nodiscard]] Result<void> validate(const Change& change) {
    if (!change.cell.is_valid()) {
        return make_error(ErrorCode::InvalidArgument, "change to an invalid cell");
    }
    if (std::to_underlying(change.layer) >= layer_count) {
        return make_error(ErrorCode::InvalidArgument,
                          std::format("unknown layer {}", std::to_underlying(change.layer)));
    }
    if (const auto* excavate = std::get_if<Excavate>(&change.payload);
        excavate != nullptr && excavate->depth <= 0) {
        return make_error(ErrorCode::InvalidArgument, "excavation depth must be positive");
    }
    if (const auto* hollow = std::get_if<Hollow>(&change.payload);
        hollow != nullptr && (hollow->top < 0 || hollow->bottom <= hollow->top)) {
        return make_error(ErrorCode::InvalidArgument,
                          "a hollow runs down from a depth of zero or more to a deeper one");
    }
    if (const auto* set = std::get_if<SetBiome>(&change.payload);
        set != nullptr && std::to_underlying(set->biome) >= biome_count) {
        return make_error(ErrorCode::InvalidArgument, "unknown biome");
    }
    return {};
}

// Adds a hollow to sorted ones that never touch, joining it to any it reaches.
void hollow(std::vector<Hollow>& hollows, Hollow added) {
    std::vector<Hollow> joined;
    joined.reserve(hollows.size() + 1);
    for (const Hollow& existing : hollows) {
        if (existing.bottom < added.top || existing.top > added.bottom) {
            joined.push_back(existing);
        } else {
            added = {.top = std::min(added.top, existing.top),
                     .bottom = std::max(added.bottom, existing.bottom)};
        }
    }
    joined.insert(std::ranges::upper_bound(joined, added.top, {}, &Hollow::top), added);
    hollows = std::move(joined);
}

void fold(CellState& state, const ChangePayload& payload) {
    std::visit(
        [&](const auto& change) {
            using Payload = std::decay_t<decltype(change)>;
            if constexpr (std::is_same_v<Payload, SetElevation>) {
                state.elevation = change.elevation;
            } else if constexpr (std::is_same_v<Payload, Excavate>) {
                state.elevation -= change.depth;
                state.excavated += change.depth;
            } else if constexpr (std::is_same_v<Payload, SetBiome>) {
                state.biome = change.biome;
            } else {
                static_assert(std::is_same_v<Payload, Hollow>);
                hollow(state.hollows, change);
            }
        },
        payload);
}

} // namespace

World::World(const PlanetConfig& config) : geology_{config}, caves_{geology_} {}

Result<void> World::apply(const Change& change) {
    if (auto valid = validate(change); !valid) {
        return valid;
    }
    cells_[change.cell.raw()].push_back({.sequence = next_sequence_++, .change = change});
    ++count_;
    return {};
}

Result<void> World::replay(std::span<const Change> changes) {
    for (std::size_t index = 0; index < changes.size(); ++index) {
        if (auto valid = validate(changes[index]); !valid) {
            return make_error(valid.error().code,
                              std::format("change {}: {}", index, valid.error().message));
        }
    }
    for (const Change& change : changes) {
        cells_[change.cell.raw()].push_back({.sequence = next_sequence_++, .change = change});
    }
    count_ += changes.size();
    return {};
}

std::size_t World::roll_back(const ChangeFilter& filter) {
    std::size_t removed = 0;
    for (auto cell = cells_.begin(); cell != cells_.end();) {
        removed += std::erase_if(cell->second,
                                 [&](const Entry& entry) { return filter.matches(entry.change); });
        cell = cell->second.empty() ? cells_.erase(cell) : std::next(cell);
    }
    count_ -= removed;
    return removed;
}

CellState World::state(CellId cell) const {
    const std::int64_t elevation = geology_.terrain().elevation(cell);
    CellState state{.elevation = elevation,
                    .biome = geology_.climate().biome(cell),
                    .temperature = geology_.climate().temperature(cell, elevation),
                    .excavated = 0,
                    .hollows = {}};
    if (cells_.empty()) {
        return state;
    }
    std::vector<const Entry*> entries;
    for (int level = cell.level(); level >= 0; --level) {
        if (const auto found = cells_.find(cell.parent(level).raw()); found != cells_.end()) {
            for (const Entry& entry : found->second) {
                entries.push_back(&entry);
            }
        }
    }
    std::ranges::sort(entries, {}, [](const Entry* entry) {
        return std::tuple{entry->change.layer, entry->change.time, entry->sequence};
    });
    for (const Entry* entry : entries) {
        fold(state, entry->change.payload);
    }
    return state;
}

std::optional<Opening> World::opening_at(CellId cell, std::int64_t depth) const {
    const CellState here = state(cell);
    const bool dug =
        depth < here.excavated ||
        std::ranges::any_of(
            here.hollows,
            [&](const Hollow& hollow) { return depth >= hollow.top && depth < hollow.bottom; });
    if (dug) {
        return Opening{};
    }
    if (const auto cave = caves_.cave_at(cell, depth)) {
        return Opening{.cave = cave};
    }
    return std::nullopt;
}

std::vector<Change> World::changes(const ChangeFilter& filter) const {
    std::vector<const Entry*> entries;
    for (const auto& [cell, cell_entries] : cells_) {
        for (const Entry& entry : cell_entries) {
            if (filter.matches(entry.change)) {
                entries.push_back(&entry);
            }
        }
    }
    std::ranges::sort(entries, {}, &Entry::sequence);
    std::vector<Change> found;
    found.reserve(entries.size());
    for (const Entry* entry : entries) {
        found.push_back(entry->change);
    }
    return found;
}

} // namespace hog::planet
