#include <hog/core/bytes.hpp>
#include <hog/sim/weather.hpp>

#include <bit>
#include <format>
#include <utility>

namespace hog::sim {

std::vector<std::byte> Weather::save() const {
    ByteWriter out;
    out.write(static_cast<std::uint8_t>(grid_.level()));
    out.write(static_cast<std::uint64_t>(time_.time_since_epoch().count()));
    const auto floats = [&](const std::vector<float>& values) {
        for (const float value : values) {
            out.write(std::bit_cast<std::uint32_t>(value));
        }
    };
    const auto vectors = [&](const std::vector<Vec2>& values) {
        for (const Vec2 value : values) {
            out.write(std::bit_cast<std::uint32_t>(value.x));
            out.write(std::bit_cast<std::uint32_t>(value.y));
        }
    };
    floats(fields_.temperature);
    floats(fields_.upper_temperature);
    floats(fields_.pressure);
    floats(fields_.vapour);
    floats(fields_.cloud);
    vectors(fields_.wind);
    vectors(fields_.jet);
    floats(fields_.evaporation);
    floats(fields_.rain);
    floats(fields_.snow);
    floats(fields_.hail);
    floats(fields_.storm);
    return std::move(out).take();
}

Result<void> Weather::load(std::uint16_t version, std::span<const std::byte> bytes) {
    if (version != save_version) {
        return make_error(ErrorCode::Unsupported,
                          std::format("weather version {} is newer than this build", version));
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
    const auto floats = [&](std::vector<float>& values) {
        for (float& value : values) {
            value = next();
        }
    };
    const auto vectors = [&](std::vector<Vec2>& values) {
        for (Vec2& value : values) {
            value.x = next();
            value.y = next();
        }
    };
    floats(loaded.temperature);
    floats(loaded.upper_temperature);
    floats(loaded.pressure);
    floats(loaded.vapour);
    floats(loaded.cloud);
    vectors(loaded.wind);
    vectors(loaded.jet);
    floats(loaded.evaporation);
    floats(loaded.rain);
    floats(loaded.snow);
    floats(loaded.hail);
    floats(loaded.storm);
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
