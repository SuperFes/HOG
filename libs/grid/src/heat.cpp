#include <hog/core/bytes.hpp>
#include <hog/grid/heat.hpp>

#include <array>
#include <bit>
#include <format>
#include <utility>

namespace hog::grid {

double Heat::over(DVec3 place) const noexcept {
    double kelvin = 0.0;
    for (const HeatSource& source : sources) {
        const DVec3 off = place - source.place;
        const double near = 1.0 - (glm::dot(off, off) / (source.radius * source.radius));
        if (near > 0.0) {
            kelvin += source.kelvin * near * near;
        }
    }
    return kelvin;
}

std::vector<std::byte> Heat::encode() const {
    ByteWriter out;
    out.write(format);
    out.write_varint(sources.size());
    for (const HeatSource& source : sources) {
        for (const double value :
             {source.place.x, source.place.y, source.place.z, source.radius, source.kelvin}) {
            out.write(std::bit_cast<std::uint64_t>(value));
        }
    }
    return std::move(out).take();
}

Result<Heat> Heat::decode(std::span<const std::byte> bytes) {
    ByteReader in{bytes};
    const auto version = in.read<std::uint8_t>();
    const auto count = in.read_varint();
    if (!version || !count) {
        return make_error(ErrorCode::Parse, "heat cut short");
    }
    if (*version != format) {
        return make_error(ErrorCode::Unsupported, std::format("heat format {}", *version));
    }
    // Each source is five doubles.
    if (*count > in.remaining() / 40) {
        return make_error(ErrorCode::Parse, "a heat source count past its bytes");
    }
    Heat heat;
    heat.sources.reserve(*count);
    for (std::uint64_t n = 0; n < *count; ++n) {
        std::array<double, 5> values{};
        for (double& value : values) {
            const auto bits = in.read<std::uint64_t>();
            if (!bits) {
                return make_error(ErrorCode::Parse, "a heat source cut short");
            }
            value = std::bit_cast<double>(*bits);
        }
        const auto [x, y, z, radius, kelvin] = values;
        if (!(radius > 0.0)) {
            return make_error(ErrorCode::Parse, "a heat source reaching nowhere");
        }
        heat.sources.push_back({.place = DVec3{x, y, z}, .radius = radius, .kelvin = kelvin});
    }
    if (auto done = in.finish(); !done) {
        return std::unexpected(std::move(done.error()));
    }
    return heat;
}

} // namespace hog::grid
