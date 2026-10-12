#include <hog/core/bytes.hpp>
#include <hog/sim/wildlife.hpp>

#include <bit>
#include <format>
#include <limits>

namespace hog::sim {

namespace {

void write_floats(ByteWriter& out, const std::vector<float>& values) {
    for (const float value : values) {
        out.write(std::bit_cast<std::uint32_t>(value));
    }
}

void read_floats(ValueReader& in, std::vector<float>& values) {
    for (float& value : values) {
        value = in.single();
    }
}

constexpr std::uint64_t never = std::numeric_limits<std::uint64_t>::max();

void write_time(ByteWriter& out, const std::optional<GameTime>& time) {
    out.write(time ? static_cast<std::uint64_t>(time->time_since_epoch().count()) : never);
}

std::optional<GameTime> read_time(ValueReader& in) {
    const auto time = in.integer<std::uint64_t>();
    if (time == never) {
        return std::nullopt;
    }
    return GameTime{GameDuration{static_cast<GameDuration::rep>(time)}};
}

void write_presence(ByteWriter& out, const Presence& presence) {
    out.write(static_cast<std::uint8_t>(presence.alive));
    out.write(static_cast<std::uint8_t>(presence.succeeded));
    write_time(out, presence.extinct);
    out.write(static_cast<std::uint32_t>(presence.emptied.size()));
    for (const auto& emptied : presence.emptied) {
        write_time(out, emptied);
    }
}

// Presence saved over another number of regions is read past.
std::optional<Presence> read_presence(ValueReader& in, std::uint32_t regions) {
    Presence presence;
    presence.alive = in.integer<std::uint8_t>() != 0;
    presence.succeeded = in.integer<std::uint8_t>() != 0;
    presence.extinct = read_time(in);
    const auto count = in.integer<std::uint32_t>();
    if (count > in.remaining() / sizeof(std::uint64_t)) {
        in.fail(Error{.code = ErrorCode::Parse,
                      .message = "wildlife lists more regions than it holds"});
        return std::nullopt;
    }
    for (std::uint32_t region = 0; region < count; ++region) {
        presence.emptied.push_back(read_time(in));
    }
    if (count != regions) {
        return std::nullopt;
    }
    return presence;
}

void write_double(ByteWriter& out, double value) {
    out.write(std::bit_cast<std::uint64_t>(value));
}

} // namespace

void Wildlife::write_watch(ByteWriter& out, std::uint32_t species) const {
    const Watched& watched = watched_[species];
    write_time(out, watched.since);
    out.write(region_count_);
    for (std::uint32_t region = 0; region < region_count_; ++region) {
        const Watch& watch = watched.regions[region];
        write_double(out, watch.baseline);
        out.write(static_cast<std::uint8_t>(watch.state));
        const std::vector<Contributor>& kills = watched.kills[region];
        out.write(static_cast<std::uint32_t>(kills.size()));
        for (const Contributor& one : kills) {
            out.write(one.hunter);
            write_double(out, one.kills);
        }
    }
}

bool Wildlife::read_watch(ValueReader& in, Watched& watched) const {
    watched.since = read_time(in).value_or(time_);
    const auto count = in.integer<std::uint32_t>();
    constexpr std::size_t least_per_region = sizeof(double) + 1 + sizeof(std::uint32_t);
    if (count > in.remaining() / least_per_region) {
        in.fail(Error{.code = ErrorCode::Parse,
                      .message = "wildlife lists more regions than it holds"});
        return false;
    }
    for (std::uint32_t region = 0; region < count && in.ok(); ++region) {
        Watch& watch = watched.regions.emplace_back();
        watch.baseline = in.twice();
        const auto state = in.integer<std::uint8_t>();
        if (state > static_cast<std::uint8_t>(Watch::State::Gone)) {
            in.fail(Error{.code = ErrorCode::Parse, .message = "wildlife holds an unknown state"});
            return false;
        }
        watch.state = static_cast<Watch::State>(state);
        const auto hunters = in.integer<std::uint32_t>();
        if (hunters > in.remaining() / (2 * sizeof(std::uint64_t))) {
            in.fail(Error{.code = ErrorCode::Parse,
                          .message = "wildlife lists more hunters than it holds"});
            return false;
        }
        std::vector<Contributor>& kills = watched.kills.emplace_back();
        for (std::uint32_t one = 0; one < hunters; ++one) {
            const auto hunter = in.integer<std::uint64_t>();
            kills.push_back(Contributor{.hunter = hunter, .kills = in.twice()});
        }
    }
    return count == region_count_;
}

std::vector<std::byte> Wildlife::save() const {
    ByteWriter out;
    out.write(static_cast<std::uint8_t>(grid_.level()));
    out.write(static_cast<std::uint64_t>(time_.time_since_epoch().count()));
    write_floats(out, fields_.forage);
    write_floats(out, fields_.carrion);
    // Species go by name, so the ecology can gain, lose and reorder them between saves.
    out.write(static_cast<std::uint32_t>(fields_.populations.size()));
    for (std::size_t species = 0; species < fields_.populations.size(); ++species) {
        const std::string& name = config_.ecology.species[species].name;
        out.write(static_cast<std::uint16_t>(name.size()));
        out.write_bytes(std::as_bytes(std::span{name}));
        write_floats(out, fields_.populations[species].count);
        write_floats(out, fields_.populations[species].fed);
        write_presence(out, presence_[species]);
        write_watch(out, static_cast<std::uint32_t>(species));
    }
    return std::move(out).take();
}

Result<void> Wildlife::load(std::uint16_t version, std::span<const std::byte> bytes) {
    if (version == 0 || version > save_version) {
        return make_error(ErrorCode::Unsupported,
                          std::format("wildlife version {} is unknown to this build", version));
    }
    ValueReader in{bytes};
    const auto level = in.integer<std::uint8_t>();
    if (in.ok() && level != grid_.level()) {
        return make_error(
            ErrorCode::InvalidArgument,
            std::format("wildlife was saved at level {}, not {}", level, grid_.level()));
    }
    const auto time = in.integer<std::uint64_t>();
    WildlifeFields loaded = fields_;
    read_floats(in, loaded.forage);
    read_floats(in, loaded.carrion);
    const auto count = in.integer<std::uint32_t>();
    // Each species holds two floats per cell, so a damaged count can't ask for more than that.
    const std::size_t species_bytes = 2 * sizeof(float) * grid_.size();
    if (count > in.remaining() / species_bytes) {
        return make_error(ErrorCode::Parse, "wildlife lists more species than it holds");
    }
    // A species no longer in the ecology is read past; one new to it keeps its start.
    Population ignored{.count = std::vector<float>(grid_.size()),
                       .fed = std::vector<float>(grid_.size())};
    // Presence and baselines a save lacks are worked out from the populations; a species it lacks
    // keeps its own.
    std::vector<std::optional<Presence>> presence(presence_.begin(), presence_.end());
    std::vector<Watched> watched = watched_;
    std::vector<bool> unwatched(watched_.size(), false);
    for (std::uint32_t read = 0; read < count && in.ok(); ++read) {
        const std::string name = in.text(in.integer<std::uint16_t>());
        const auto species = config_.ecology.find(name);
        Population& into = species ? loaded.populations[*species] : ignored;
        read_floats(in, into.count);
        read_floats(in, into.fed);
        std::optional<Presence> present =
            version >= 2 ? read_presence(in, region_count_) : std::nullopt;
        Watched seen;
        const bool usable = version >= 3 && read_watch(in, seen);
        if (species) {
            presence[*species] = std::move(present);
            unwatched[*species] = !usable;
            if (usable) {
                watched[*species] = std::move(seen);
            }
        }
    }
    if (auto finished = in.finish(); !finished) {
        return finished;
    }
    const GameTime loaded_time{GameDuration{static_cast<GameDuration::rep>(time)}};
    const auto held_then = held(loaded);
    for (std::uint32_t species = 0; species < presence_.size(); ++species) {
        std::optional<Presence>& kept = presence[species];
        presence_[species] =
            kept ? std::move(*kept) : present(species, held_then[species], loaded_time);
        standing_[species] = standing(presence_[species]);
        watched_[species] = unwatched[species] ? watching(held_then[species], loaded_time)
                                               : std::move(watched[species]);
    }
    fields_ = std::move(loaded);
    time_ = loaded_time;
    return {};
}

} // namespace hog::sim
