#include <hog/core/bytes.hpp>
#include <hog/sim/forest.hpp>

#include <bit>
#include <format>
#include <utility>

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

} // namespace

std::vector<std::byte> Forest::save() const {
    ByteWriter out;
    out.write(static_cast<std::uint8_t>(grid_.level()));
    out.write(static_cast<std::uint64_t>(time_.time_since_epoch().count()));
    write_floats(out, fields_.biomass);
    write_floats(out, fields_.dryness);
    write_floats(out, fields_.fire);
    return std::move(out).take();
}

Result<void> Forest::load(std::uint16_t version, std::span<const std::byte> bytes) {
    if (version == 0 || version > save_version) {
        return make_error(ErrorCode::Unsupported,
                          std::format("forest version {} is unknown to this build", version));
    }
    ValueReader in{bytes};
    const auto level = in.integer<std::uint8_t>();
    if (in.ok() && level != grid_.level()) {
        return make_error(
            ErrorCode::InvalidArgument,
            std::format("forest was saved at level {}, not {}", level, grid_.level()));
    }
    const auto time = in.integer<std::uint64_t>();
    ForestFields loaded = fields_;
    read_floats(in, loaded.biomass);
    read_floats(in, loaded.dryness);
    read_floats(in, loaded.fire);
    if (auto finished = in.finish(); !finished) {
        return finished;
    }
    fields_ = std::move(loaded);
    time_ = GameTime{GameDuration{static_cast<GameDuration::rep>(time)}};
    return {};
}

} // namespace hog::sim
