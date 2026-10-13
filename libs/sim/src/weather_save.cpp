#include "weather_stages.hpp"

#include <hog/core/bytes.hpp>
#include <hog/sim/weather.hpp>

#include <bit>
#include <concepts>
#include <cstdint>
#include <format>
#include <optional>
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
    visit(fields.front, 2);
    visit(fields.recent_temperature, 3);
    visit(fields.recent_precipitation, 3);
}

void read_value(ValueReader& in, float& value) {
    value = in.single();
}

void read_value(ValueReader& in, Vec2& value) {
    value.x = in.single();
    value.y = in.single();
}

Result<std::vector<StormSystem>> read_systems(ValueReader& in) {
    const auto count = in.integer<std::uint32_t>();
    // A damaged count can't ask for more systems than the bytes left could hold.
    constexpr std::size_t system_bytes = 44;
    if (count > in.remaining() / system_bytes) {
        return make_error(ErrorCode::Parse, "weather lists more storm systems than it holds");
    }
    std::vector<StormSystem> systems(count);
    for (StormSystem& system : systems) {
        system.id = in.integer<std::uint32_t>();
        system.centre.x = in.twice();
        system.centre.y = in.twice();
        system.centre.z = in.twice();
        system.depth = in.single();
        system.radius = in.single();
        system.age = in.single();
        system.life = in.single();
    }
    return systems;
}

template <typename Fields, typename Visit>
void each_sea_field(Fields& fields, Visit visit) {
    visit(fields.temperature);
    visit(fields.current);
    visit(fields.stress);
    visit(fields.stream);
}

void read_value(ValueReader& in, double& value) {
    value = in.twice();
}

void write_double(ByteWriter& out, double value) {
    out.write(std::bit_cast<std::uint64_t>(value));
}

void write_float(ByteWriter& out, float value) {
    out.write(std::bit_cast<std::uint32_t>(value));
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
    out.write(next_system_);
    out.write(static_cast<std::uint32_t>(systems_.size()));
    for (const StormSystem& system : systems_) {
        out.write(system.id);
        write_double(out, system.centre.x);
        write_double(out, system.centre.y);
        write_double(out, system.centre.z);
        write_float(out, system.depth);
        write_float(out, system.radius);
        write_float(out, system.age);
        write_float(out, system.life);
    }
    each_sea_field(ocean_.fields(), [&](const auto& values) {
        for (const auto value : values) {
            using Value = std::remove_cvref_t<decltype(value)>;
            if constexpr (std::is_same_v<Value, Vec2>) {
                write_float(out, value.x);
                write_float(out, value.y);
            } else if constexpr (std::is_same_v<Value, double>) {
                write_double(out, value);
            } else {
                write_float(out, value);
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
    ValueReader in{bytes};
    const auto level = in.integer<std::uint8_t>();
    if (in.ok() && level != grid_.level()) {
        return make_error(
            ErrorCode::InvalidArgument,
            std::format("weather was saved at level {}, not {}", level, grid_.level()));
    }
    const auto time = in.integer<std::uint64_t>();
    WeatherFields loaded{grid_.size()};
    each_field(loaded, [&](auto& values, int since) {
        if (std::cmp_less_equal(since, version)) {
            for (auto& value : values) {
                read_value(in, value);
            }
        }
    });
    std::uint32_t next_system = 1;
    std::vector<StormSystem> systems;
    if (version >= 2) {
        next_system = in.integer<std::uint32_t>();
        auto read = read_systems(in);
        if (!read) {
            return std::unexpected(std::move(read.error()));
        }
        systems = std::move(*read);
    }
    OceanFields sea{grid_.size()};
    if (version >= 4) {
        each_sea_field(sea, [&](auto& values) {
            for (auto& value : values) {
                read_value(in, value);
            }
        });
    }
    if (auto finished = in.finish(); !finished) {
        return finished;
    }
    if (version < 3) {
        for (std::uint32_t index = 0; index < grid_.size(); ++index) {
            loaded.recent_temperature[index] = loaded.temperature[index];
            loaded.recent_precipitation[index] = detail::usual_precipitation(ground_[index]);
        }
    }
    fields_ = std::move(loaded);
    systems_ = std::move(systems);
    next_system_ = next_system;
    time_ = GameTime{GameDuration{static_cast<GameDuration::rep>(time)}};
    if (version >= 4) {
        ocean_.set_fields(grid_, std::move(sea));
    } else {
        spin_up_ocean();
    }
    return {};
}

} // namespace hog::sim
