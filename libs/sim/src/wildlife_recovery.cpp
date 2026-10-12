#include "wildlife_stages.hpp"

#include <hog/core/hash.hpp>

#include <limits>

namespace hog::sim {

namespace {

// The food a species most prefers, which it shares a niche with any rival over.
const Food* main_food(const Species& species) {
    const auto found = std::ranges::max_element(species.diet, {}, &Food::preference);
    return found == species.diet.end() ? nullptr : &*found;
}

bool same_food(const Food& a, const Food& b) {
    return a.kind == b.kind && (a.kind != Food::Kind::Prey || a.prey == b.prey);
}

double sum(std::span<const double> values) {
    double total = 0.0;
    for (const double value : values) {
        total += value;
    }
    return total;
}

constexpr std::uint64_t everywhere = std::numeric_limits<std::uint64_t>::max();

} // namespace

std::vector<std::vector<double>> Wildlife::held(const WildlifeFields& fields) const {
    std::vector<std::vector<double>> held(fields.populations.size(),
                                          std::vector<double>(region_count_));
    for (std::size_t kind = 0; kind < held.size(); ++kind) {
        const std::vector<float>& count = fields.populations[kind].count;
        for (std::uint32_t cell = 0; cell < grid_.size(); ++cell) {
            held[kind][regions_[cell]] += static_cast<double>(count[cell]);
        }
    }
    return held;
}

Presence Wildlife::present(std::uint32_t species, std::span<const double> held, GameTime at) const {
    Presence presence{.emptied = std::vector<std::optional<GameTime>>(region_count_),
                      .extinct = std::nullopt,
                      .alive = sum(held) > 0.0,
                      .succeeded = false};
    for (std::uint32_t region = 0; region < region_count_; ++region) {
        if (livable_[species][region] && held[region] <= 0.0) {
            presence.emptied[region] = at;
        }
    }
    return presence;
}

GameDuration Wildlife::wait(std::uint32_t species, std::uint64_t place, GameTime since,
                            double earliest, double latest) const {
    const double share = detail::fraction(
        hash_values(config_.seed, hash_string(config_.ecology.species[species].name), place,
                    static_cast<std::uint64_t>(since.time_since_epoch().count())));
    const double years = earliest + ((latest - earliest) * share);
    return std::chrono::duration_cast<GameDuration>(Seconds{years * days_per_year * 86'400.0});
}

template <typename Where>
std::optional<std::uint32_t> Wildlife::best_cell(const detail::LifeContext& context,
                                                 std::uint32_t species, Where where) const {
    std::optional<std::uint32_t> best;
    double most = 0.0;
    for (std::uint32_t cell = 0; cell < grid_.size(); ++cell) {
        if (!where(cell)) {
            continue;
        }
        const double appeal = detail::appeal(context, fields_, species, cell);
        if (appeal > most) {
            most = appeal;
            best = cell;
        }
    }
    return best;
}

void Wildlife::recover(const detail::LifeContext& context, GameTime at) {
    const auto& species = config_.ecology.species;
    std::vector<std::vector<double>> held_now = held(fields_);
    for (std::uint32_t kind = 0; kind < species.size(); ++kind) {
        track(kind, held_now[kind], at);
    }
    for (std::uint32_t kind = 0; kind < species.size(); ++kind) {
        if (presence_[kind].alive) {
            found(context, kind, held_now, at);
        } else {
            outlive(context, kind, at);
        }
    }
}

void Wildlife::track(std::uint32_t species, std::span<const double> held, GameTime at) {
    Presence& presence = presence_[species];
    if (sum(held) > 0.0 && !presence.alive) {
        // Back from extinction: every empty region's wait starts over.
        presence = present(species, held, at);
        return;
    }
    if (sum(held) <= 0.0 && presence.alive) {
        presence.extinct = at;
    }
    presence.alive = sum(held) > 0.0;
    for (std::uint32_t region = 0; region < region_count_; ++region) {
        if (held[region] > 0.0) {
            presence.emptied[region] = std::nullopt;
        } else if (livable_[species][region] && !presence.emptied[region]) {
            presence.emptied[region] = at;
        }
    }
}

bool Wildlife::open_niche(std::uint32_t species, std::uint32_t region,
                          const std::vector<std::vector<double>>& held) const {
    const auto& all = config_.ecology.species;
    const Food* food = main_food(all[species]);
    if (food == nullptr) {
        return true;
    }
    for (std::uint32_t rival = 0; rival < all.size(); ++rival) {
        const Food* theirs = main_food(all[rival]);
        if (rival != species && theirs != nullptr && same_food(*food, *theirs) &&
            held[rival][region] >= all[rival].recovery.founders) {
            return false;
        }
    }
    return true;
}

void Wildlife::found(const detail::LifeContext& context, std::uint32_t species,
                     std::vector<std::vector<double>>& held, GameTime at) {
    const Recovery& recovery = config_.ecology.species[species].recovery;
    Presence& presence = presence_[species];
    if (recovery.founders <= 0.0) {
        return;
    }
    for (std::uint32_t region = 0; region < region_count_; ++region) {
        const std::optional<GameTime> emptied = presence.emptied[region];
        if (!emptied ||
            at < *emptied + wait(species, region, *emptied, recovery.earliest, recovery.latest)) {
            continue;
        }
        if (!open_niche(species, region, held)) {
            // Rivals hold the ground; founders try again after another wait.
            presence.emptied[region] = at;
            continue;
        }
        // With nothing to draw them yet, they wait until there is.
        const auto cell = best_cell(context, species,
                                    [&](std::uint32_t where) { return regions_[where] == region; });
        if (cell) {
            add(species, *cell, static_cast<float>(recovery.founders));
            held[species][region] += recovery.founders;
            presence.emptied[region] = std::nullopt;
        }
    }
}

void Wildlife::outlive(const detail::LifeContext& context, std::uint32_t species, GameTime at) {
    const Species& one = config_.ecology.species[species];
    const Recovery& recovery = one.recovery;
    Presence& presence = presence_[species];
    if (!presence.extinct || recovery.after_extinction == Recovery::After::Admin ||
        at < *presence.extinct + wait(species, everywhere, *presence.extinct,
                                      recovery.extinct_earliest, recovery.extinct_latest)) {
        return;
    }
    if (recovery.after_extinction == Recovery::After::Relict) {
        if (const auto cell = best_cell(context, species, [](std::uint32_t) { return true; })) {
            add(species, *cell, static_cast<float>(recovery.founders));
        }
        return;
    }
    if (presence.succeeded) {
        return;
    }
    // The successor arrives where the lost species could have lived.
    const std::uint32_t successor = recovery.successor;
    const auto cell = best_cell(context, successor, [&](std::uint32_t where) {
        return detail::passable(one, habitat_[where]) &&
               one.habitat.at(static_cast<std::size_t>(habitat_[where].biome)) > 0.0;
    });
    if (cell) {
        add(successor, *cell,
            static_cast<float>(config_.ecology.species[successor].recovery.founders));
        presence.succeeded = true;
    }
}

} // namespace hog::sim
