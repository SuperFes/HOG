#include <hog/core/bytes.hpp>
#include <hog/grid/water_bodies.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <format>
#include <limits>
#include <utility>

namespace hog::grid {

namespace {

enum class Kind : std::uint8_t { Sea = 0, Lake = 1, River = 2 };

void write(ByteWriter& out, double value) {
    out.write(std::bit_cast<std::uint64_t>(value));
}

void write(ByteWriter& out, const Season& season) {
    write(out, season.rise);
    write(out, season.peak);
}

// Reads doubles in order, keeping the first failure.
class Doubles {
public:
    explicit Doubles(ByteReader& in) noexcept : in_{&in} {}

    double next() {
        auto bits = in_->read<std::uint64_t>();
        if (!bits) {
            ok_ = false;
            return 0.0;
        }
        return std::bit_cast<double>(*bits);
    }
    Season season() {
        const double rise = next();
        return {.rise = rise, .peak = next()};
    }
    [[nodiscard]] bool ok() const noexcept { return ok_; }

private:
    ByteReader* in_;
    bool ok_ = true;
};

Result<Body> read_body(ByteReader& in) {
    const auto kind = in.read<std::uint8_t>();
    if (!kind) {
        return make_error(ErrorCode::Parse, "a water body cut short");
    }
    Doubles doubles{in};
    switch (static_cast<Kind>(*kind)) {
    case Kind::Sea: {
        const Sea sea{.level = doubles.next()};
        if (doubles.ok()) {
            return sea;
        }
        break;
    }
    case Kind::Lake: {
        const double level = doubles.next();
        const Lake lake{.level = level, .season = doubles.season()};
        if (doubles.ok()) {
            return lake;
        }
        break;
    }
    case Kind::River: {
        River river;
        river.peak = doubles.next();
        const auto count = in.read_varint();
        if (!doubles.ok() || !count) {
            break;
        }
        // Each point is six doubles.
        if (*count < 2 || *count > in.remaining() / 48) {
            return make_error(ErrorCode::Parse, "a river's points past its bytes, or too few");
        }
        river.course.resize(*count);
        for (River::Point& point : river.course) {
            const double x = doubles.next();
            const double y = doubles.next();
            const double z = doubles.next();
            const double level = doubles.next();
            const double flow = doubles.next();
            point = {.place = DVec3{x, y, z}, .level = level, .flow = flow, .rise = doubles.next()};
        }
        if (doubles.ok()) {
            return river;
        }
        break;
    }
    default:
        return make_error(ErrorCode::Parse, std::format("water body kind {}", *kind));
    }
    return make_error(ErrorCode::Parse, "a water body cut short");
}

} // namespace

double running_speed(double flow) noexcept {
    if (flow <= 0.0) {
        return 0.0;
    }
    // Flow over the channel's width times its depth.
    return std::pow(flow, 0.1) / (3.0 * 0.27);
}

River::Along River::along(DVec3 place) const noexcept {
    Along best{.off = std::numeric_limits<double>::infinity()};
    double from_head = 0.0;
    for (std::size_t n = 0; n + 1 < course.size(); ++n) {
        const Point& a = course[n];
        const Point& b = course[n + 1];
        const DVec3 run = b.place - a.place;
        const double length = glm::length(run);
        const double t = std::clamp(glm::dot(place - a.place, run) / (length * length), 0.0, 1.0);
        const double off = glm::distance(place, a.place + (run * t));
        if (off < best.off) {
            best = {.level = a.level + ((b.level - a.level) * t),
                    .flow = a.flow + ((b.flow - a.flow) * t),
                    .rise = a.rise + ((b.rise - a.rise) * t),
                    .downstream = run / length,
                    .from_head = from_head + (length * t),
                    .off = off};
        }
        from_head += length;
    }
    return best;
}

std::vector<std::byte> WaterBodies::encode() const {
    ByteWriter out;
    out.write(format);
    out.write_varint(bodies.size());
    for (const auto& [id, body] : bodies) {
        out.write_varint(id);
        if (const auto* sea = std::get_if<Sea>(&body)) {
            out.write(std::to_underlying(Kind::Sea));
            write(out, sea->level);
        } else if (const auto* lake = std::get_if<Lake>(&body)) {
            out.write(std::to_underlying(Kind::Lake));
            write(out, lake->level);
            write(out, lake->season);
        } else if (const auto* river = std::get_if<River>(&body)) {
            out.write(std::to_underlying(Kind::River));
            write(out, river->peak);
            out.write_varint(river->course.size());
            for (const River::Point& point : river->course) {
                write(out, point.place.x);
                write(out, point.place.y);
                write(out, point.place.z);
                write(out, point.level);
                write(out, point.flow);
                write(out, point.rise);
            }
        }
    }
    return std::move(out).take();
}

Result<WaterBodies> WaterBodies::decode(std::span<const std::byte> bytes) {
    ByteReader in{bytes};
    const auto version = in.read<std::uint8_t>();
    const auto count = in.read_varint();
    if (!version || !count) {
        return make_error(ErrorCode::Parse, "water bodies cut short");
    }
    if (*version != format) {
        return make_error(ErrorCode::Unsupported, std::format("water bodies format {}", *version));
    }
    if (*count > in.remaining()) {
        return make_error(ErrorCode::Parse, "a water body count past its bytes");
    }
    WaterBodies table;
    for (std::uint64_t n = 0; n < *count; ++n) {
        const auto id = in.read_varint();
        if (!id) {
            return make_error(ErrorCode::Parse, "a water body cut short");
        }
        if (*id == 0 || *id > std::numeric_limits<Water::Value>::max()) {
            return make_error(ErrorCode::Parse, std::format("water body id {}", *id));
        }
        auto body = read_body(in);
        if (!body) {
            return std::unexpected(std::move(body.error()));
        }
        if (!table.bodies.emplace(static_cast<Water::Value>(*id), std::move(*body)).second) {
            return make_error(ErrorCode::Parse, std::format("water body {} twice", *id));
        }
    }
    if (auto done = in.finish(); !done) {
        return std::unexpected(std::move(done.error()));
    }
    return table;
}

} // namespace hog::grid
