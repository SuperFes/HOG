#include <hog/core/bytes.hpp>
#include <hog/weather/state.hpp>

#include <bit>
#include <cmath>
#include <utility>

namespace hog::weather {
namespace {

[[nodiscard]] Error damaged(std::string message) {
    return {.code = ErrorCode::Parse, .message = "air state: " + std::move(message)};
}

} // namespace

std::vector<std::byte> encode_state(const Field& field) {
    ByteWriter out;
    out.write(static_cast<std::uint32_t>(field.grid.level));
    out.write(static_cast<std::uint32_t>(field.grid.layers));
    out.write(static_cast<std::uint64_t>(field.air.size()));
    out.write(static_cast<std::uint64_t>(field.surface.size()));
    const auto single = [&](float value) { out.write(std::bit_cast<std::uint32_t>(value)); };
    for (const Air& air : field.air) {
        single(air.temperature);
        single(air.vapour);
        single(air.cloud);
        single(air.rain);
        single(air.wind.x);
        single(air.wind.y);
        single(air.wind.z);
    }
    for (const float temperature : field.surface) {
        single(temperature);
    }
    return std::move(out).take();
}

Result<void> decode_state(Field& field, std::span<const std::byte> bytes) {
    ValueReader in{bytes};
    const auto level = in.integer<std::uint32_t>();
    const auto layers = in.integer<std::uint32_t>();
    const auto cells = in.integer<std::uint64_t>();
    const auto columns = in.integer<std::uint64_t>();
    if (!in.ok()) {
        return in.finish();
    }
    if (std::cmp_not_equal(level, field.grid.level) ||
        std::cmp_not_equal(layers, field.grid.layers) || cells != field.air.size() ||
        columns != field.surface.size()) {
        return std::unexpected(damaged("written for another grid"));
    }
    std::vector<Air> air(field.air.size());
    std::vector<float> surface(field.surface.size());
    bool finite = true;
    const auto next = [&] {
        const float value = in.single();
        finite = finite && std::isfinite(value);
        return value;
    };
    for (Air& cell : air) {
        cell.temperature = next();
        cell.vapour = next();
        cell.cloud = next();
        cell.rain = next();
        cell.wind = {next(), next(), next()};
    }
    for (float& temperature : surface) {
        temperature = next();
    }
    if (auto done = in.finish(); !done) {
        return done;
    }
    if (!finite) {
        return std::unexpected(damaged("not a number in it"));
    }
    field.air = std::move(air);
    field.surface = std::move(surface);
    return {};
}

} // namespace hog::weather
