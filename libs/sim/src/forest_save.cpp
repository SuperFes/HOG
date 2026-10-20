#include <hog/core/bytes.hpp>
#include <hog/sim/forest.hpp>

#include <algorithm>
#include <bit>
#include <cstdint>
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
    write_floats(out, fields_.established);
    write_floats(out, fields_.failing);
    write_floats(out, fields_.dead);
    write_floats(out, fields_.charred);
    write_floats(out, fields_.shortfall);
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
    // The first version kept no fates: its living all counted old growth, nothing dead.
    if (version >= 2) {
        read_floats(in, loaded.established);
        read_floats(in, loaded.failing);
        read_floats(in, loaded.dead);
        read_floats(in, loaded.charred);
        read_floats(in, loaded.shortfall);
    } else {
        for (std::uint32_t cell = 0; cell < grid_.size(); ++cell) {
            const double holds = capacity(cell);
            loaded.established[cell] =
                holds > 0.0 ? static_cast<float>(
                                  std::min(static_cast<double>(loaded.biomass[cell]) / holds, 1.0))
                            : 1.0F;
        }
        std::ranges::fill(loaded.failing, 0.0F);
        std::ranges::fill(loaded.dead, 0.0F);
        std::ranges::fill(loaded.charred, 0.0F);
        std::ranges::fill(loaded.shortfall, 0.0F);
    }
    if (auto finished = in.finish(); !finished) {
        return finished;
    }
    fields_ = std::move(loaded);
    time_ = GameTime{GameDuration{static_cast<GameDuration::rep>(time)}};
    return {};
}

} // namespace hog::sim
