#include "save_reader.hpp"

#include <hog/core/bytes.hpp>
#include <hog/sim/wildlife.hpp>

#include <bit>
#include <format>
#include <limits>

namespace hog::sim {

namespace {

using detail::Reader;

void write_floats(ByteWriter& out, const std::vector<float>& values) {
    for (const float value : values) {
        out.write(std::bit_cast<std::uint32_t>(value));
    }
}

void read_floats(Reader& in, std::vector<float>& values) {
    for (float& value : values) {
        value = in.single();
    }
}

constexpr std::uint64_t never = std::numeric_limits<std::uint64_t>::max();

void write_time(ByteWriter& out, const std::optional<GameTime>& time) {
    out.write(time ? static_cast<std::uint64_t>(time->time_since_epoch().count()) : never);
}

std::optional<GameTime> read_time(Reader& in) {
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
std::optional<Presence> read_presence(Reader& in, std::uint32_t regions) {
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

} // namespace

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
    }
    return std::move(out).take();
}

Result<void> Wildlife::load(std::uint16_t version, std::span<const std::byte> bytes) {
    if (version == 0 || version > save_version) {
        return make_error(ErrorCode::Unsupported,
                          std::format("wildlife version {} is unknown to this build", version));
    }
    Reader in{bytes};
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
    // Presence a save lacks is worked out from the populations; a species it lacks keeps its own.
    std::vector<std::optional<Presence>> presence(presence_.begin(), presence_.end());
    for (std::uint32_t read = 0; read < count && in.ok(); ++read) {
        const std::string name = in.text(in.integer<std::uint16_t>());
        const auto species = config_.ecology.find(name);
        Population& into = species ? loaded.populations[*species] : ignored;
        read_floats(in, into.count);
        read_floats(in, into.fed);
        std::optional<Presence> present =
            version >= 2 ? read_presence(in, region_count_) : std::nullopt;
        if (species) {
            presence[*species] = std::move(present);
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
    }
    fields_ = std::move(loaded);
    time_ = loaded_time;
    return {};
}

} // namespace hog::sim
