#include <hog/core/bytes.hpp>
#include <hog/sim/weather.hpp>

#include <bit>
#include <cstdint>
#include <format>
#include <type_traits>
#include <utility>

namespace hog::sim {

namespace {

// Every saved field in order, with the version that added it. Fields only ever join at the end,
// and an older save leaves the newer ones at zero.
template <typename Fields, typename Visit>
void each_field(Fields& fields, Visit visit) {
    visit(fields.temperature, 1);
    visit(fields.upper_temperature, 1);
    visit(fields.pressure, 1);
    visit(fields.vapour, 1);
    visit(fields.cloud, 1);
    visit(fields.wind, 1);
    visit(fields.jet, 1);
    visit(fields.evaporation, 1);
    visit(fields.rain, 1);
    visit(fields.snow, 1);
    visit(fields.hail, 1);
    visit(fields.storm, 1);
    visit(fields.lift, 2);
}

} // namespace

std::vector<std::byte> Weather::save() const {
    ByteWriter out;
    out.write(static_cast<std::uint8_t>(grid_.level()));
    out.write(static_cast<std::uint64_t>(time_.time_since_epoch().count()));
    each_field(fields_, [&](const auto& values, int /*since*/) {
        for (const auto value : values) {
            if constexpr (std::is_same_v<std::remove_cvref_t<decltype(value)>, Vec2>) {
                out.write(std::bit_cast<std::uint32_t>(value.x));
                out.write(std::bit_cast<std::uint32_t>(value.y));
            } else {
                out.write(std::bit_cast<std::uint32_t>(value));
            }
        }
    });
    return std::move(out).take();
}

Result<void> Weather::load(std::uint16_t version, std::span<const std::byte> bytes) {
    if (version == 0 || version > save_version) {
        return make_error(ErrorCode::Unsupported,
                          std::format("weather version {} is unknown to this build", version));
    }
    ByteReader in{bytes};
    const auto level = in.read<std::uint8_t>();
    if (!level) {
        return std::unexpected(level.error());
    }
    if (*level != grid_.level()) {
        return make_error(
            ErrorCode::InvalidArgument,
            std::format("weather was saved at level {}, not {}", *level, grid_.level()));
    }
    const auto time = in.read<std::uint64_t>();
    if (!time) {
        return std::unexpected(time.error());
    }
    WeatherFields loaded{grid_.size()};
    Result<void> read{};
    const auto next = [&] {
        auto bits = read ? in.read<std::uint32_t>() : Result<std::uint32_t>{0U};
        if (!bits) {
            read = std::unexpected(std::move(bits.error()));
        }
        return std::bit_cast<float>(bits.value_or(0U));
    };
    each_field(loaded, [&](auto& values, int since) {
        if (std::cmp_greater(since, version)) {
            return;
        }
        for (auto& value : values) {
            if constexpr (std::is_same_v<std::remove_cvref_t<decltype(value)>, Vec2>) {
                value.x = next();
                value.y = next();
            } else {
                value = next();
            }
        }
    });
    if (!read) {
        return read;
    }
    if (auto finished = in.finish(); !finished) {
        return finished;
    }
    fields_ = std::move(loaded);
    time_ = GameTime{GameDuration{static_cast<GameDuration::rep>(*time)}};
    return {};
}

} // namespace hog::sim
