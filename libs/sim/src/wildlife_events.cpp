#include <hog/sim/wildlife.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

namespace hog::sim {

namespace {

// Kills remembered as fewer than this are forgotten.
constexpr double forgotten = 0.01;

void remember(std::vector<Contributor>& kills, Hunter hunter, double count) {
    const auto found = std::ranges::find(kills, hunter, &Contributor::hunter);
    if (found == kills.end()) {
        kills.push_back(Contributor{.hunter = hunter, .kills = count});
    } else {
        found->kills += count;
    }
}

double years_of(GameDuration duration) {
    return Seconds{duration}.count() / (days_per_year * 86'400.0);
}

} // namespace

std::string_view to_string(EcologyEvent::Kind kind) noexcept {
    switch (kind) {
    case EcologyEvent::Kind::Collapse:
        return "collapse";
    case EcologyEvent::Kind::Overpopulation:
        return "overpopulation";
    case EcologyEvent::Kind::LocalExtinction:
        return "local extinction";
    case EcologyEvent::Kind::Return:
        return "return";
    case EcologyEvent::Kind::Extinction:
        return "extinction";
    }
    return "unknown";
}

float Wildlife::kill(std::uint32_t species, std::uint32_t cell, float count, Hunter hunter) {
    const float taken = take(species, cell, count);
    if (taken > 0.0F) {
        remember(watched_[species].kills[regions_[cell]], hunter, static_cast<double>(taken));
    }
    return taken;
}

std::vector<EcologyEvent> Wildlife::drain_events() {
    return std::exchange(events_, {});
}

Wildlife::Watched Wildlife::watching(std::span<const double> held, GameTime since) const {
    Watched watched{.since = since,
                    .regions = std::vector<Watch>(region_count_),
                    .kills = std::vector<std::vector<Contributor>>(region_count_)};
    for (std::uint32_t region = 0; region < region_count_; ++region) {
        watched.regions[region].baseline = held[region];
    }
    return watched;
}

Wildlife::Standing Wildlife::standing(const Presence& presence) {
    if (presence.alive) {
        return Standing::Alive;
    }
    return presence.extinct ? Standing::Extinct : Standing::Absent;
}

std::vector<Contributor> Wildlife::named(std::vector<Contributor> kills) const {
    double total = 0.0;
    for (const Contributor& one : kills) {
        total += one.kills;
    }
    std::ranges::stable_sort(kills, std::ranges::greater{}, &Contributor::kills);
    std::size_t keep = 0;
    while (keep < kills.size() && keep < config_.named &&
           kills[keep].kills >= config_.named_share * total) {
        ++keep;
    }
    kills.resize(keep);
    return kills;
}

void Wildlife::announce(EcologyEvent::Kind kind, std::uint32_t species, std::uint32_t region,
                        GameTime at, double count, double baseline) {
    std::vector<Contributor> kills;
    if (kind != EcologyEvent::Kind::Return) {
        if (region != EcologyEvent::everywhere) {
            kills = watched_[species].kills[region];
        } else {
            for (const auto& here : watched_[species].kills) {
                for (const Contributor& one : here) {
                    remember(kills, one.hunter, one.kills);
                }
            }
        }
    }
    events_.push_back(EcologyEvent{.kind = kind,
                                   .species = species,
                                   .region = region,
                                   .at = at,
                                   .count = count,
                                   .baseline = baseline,
                                   .hunters = named(std::move(kills))});
}

void Wildlife::watch(std::uint32_t species, std::uint32_t region, double held, GameTime at) {
    using State = Watch::State;
    using Kind = EcologyEvent::Kind;
    const Events& events = config_.ecology.species[species].events;
    Watch& watch = watched_[species].regions[region];
    const double baseline = watch.baseline;
    const bool settled = at >= watched_[species].since +
                                   std::chrono::duration_cast<GameDuration>(
                                       Seconds{events.baseline_years * days_per_year * 86'400.0});
    const auto tell = [&](Kind kind) { announce(kind, species, region, at, held, baseline); };
    if (watch.state == State::Gone) {
        // A few strays don't make a return.
        if (held >= events.least * events.collapse) {
            tell(Kind::Return);
            // Newcomers are few next to the baseline; that isn't news.
            watch.state = held < events.collapse * baseline ? State::Collapsed : State::Usual;
        }
        return;
    }
    // Regions that never held many come and go unremarked.
    if (baseline <= 0.0 || baseline < events.least) {
        return;
    }
    if (held <= 0.0) {
        tell(Kind::LocalExtinction);
        watch.state = State::Gone;
        return;
    }
    // Once reported, a region settles back to usual halfway (by ratio) to its baseline.
    switch (watch.state) {
    case State::Usual:
        if (!settled) {
            break;
        }
        if (held < events.collapse * baseline) {
            tell(Kind::Collapse);
            watch.state = State::Collapsed;
        } else if (held > events.overpopulation * baseline) {
            tell(Kind::Overpopulation);
            watch.state = State::Overrun;
        }
        break;
    case State::Collapsed:
        if (held >= std::sqrt(events.collapse) * baseline) {
            watch.state = State::Usual;
        }
        break;
    case State::Overrun:
        if (held <= std::sqrt(events.overpopulation) * baseline) {
            watch.state = State::Usual;
        }
        break;
    case State::Gone:
        break;
    }
}

void Wildlife::report(GameTime at) {
    const auto held_now = held(fields_);
    const double years = years_of(config_.step);
    const double fade = std::exp2(-years / config_.kill_half_life);
    for (std::uint32_t species = 0; species < held_now.size(); ++species) {
        // Until the baseline has followed for its full span, it's the average so far.
        const double watched_years = years_of(at - watched_[species].since);
        const double follow = std::max(
            1.0 - std::exp(-years / config_.ecology.species[species].events.baseline_years),
            years / (watched_years + years));
        double held_all = 0.0;
        double baseline_all = 0.0;
        for (std::uint32_t region = 0; region < region_count_; ++region) {
            const double held_here = held_now[species][region];
            watch(species, region, held_here, at);
            Watch& watch = watched_[species].regions[region];
            held_all += held_here;
            baseline_all += watch.baseline;
            watch.baseline += (held_here - watch.baseline) * follow;
            std::vector<Contributor>& kills = watched_[species].kills[region];
            for (Contributor& one : kills) {
                one.kills *= fade;
            }
            std::erase_if(kills, [](const Contributor& one) { return one.kills < forgotten; });
        }
        const Standing now = standing(presence_[species]);
        if (standing_[species] == Standing::Alive && now == Standing::Extinct) {
            announce(EcologyEvent::Kind::Extinction, species, EcologyEvent::everywhere, at, 0.0,
                     baseline_all);
        } else if (standing_[species] == Standing::Extinct && now == Standing::Alive) {
            announce(EcologyEvent::Kind::Return, species, EcologyEvent::everywhere, at, held_all,
                     baseline_all);
        }
        standing_[species] = now;
    }
}

} // namespace hog::sim
