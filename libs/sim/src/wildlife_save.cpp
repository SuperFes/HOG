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
    for (std::uint32_t read = 0; read < count && in.ok(); ++read) {
        const std::string name = in.text(in.integer<std::uint16_t>());
        const auto species = config_.ecology.find(name);
        Population& into = species ? loaded.populations[*species] : ignored;
        read_floats(in, into.count);
        read_floats(in, into.fed);
    }
    if (auto finished = in.finish(); !finished) {
        return finished;
    }
    fields_ = std::move(loaded);
    time_ = GameTime{GameDuration{static_cast<GameDuration::rep>(time)}};
    return {};
}

} // namespace hog::sim
