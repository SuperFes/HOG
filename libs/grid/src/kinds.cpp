#include <hog/grid/kinds.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>

namespace hog::grid {

namespace {

constexpr double full = 65535.0;

// Where each of eight children's middle lies from their parent's, in the parent's sides.
DVec3 offset(std::size_t octant) noexcept {
    return DVec3{(octant & 1U) != 0 ? 0.25 : -0.25, (octant & 2U) != 0 ? 0.25 : -0.25,
                 (octant & 4U) != 0 ? 0.25 : -0.25};
}

// How far a node reaches along `normal` from its middle, in its sides.
double reach(DVec3 normal) noexcept {
    return (std::abs(normal.x) + std::abs(normal.y) + std::abs(normal.z)) / 2.0;
}

double sign(double value) noexcept {
    return value < 0.0 ? -1.0 : 1.0;
}

std::uint16_t flattened(double share) noexcept {
    return static_cast<std::uint16_t>(std::round(((share * 0.5) + 0.5) * full));
}

// A plane between the solid and the open children of eight none of which it crosses.
Surface between(const std::array<Surface, 8>& children) noexcept {
    DVec3 toward_open{0.0};
    int open = 0;
    for (std::size_t k = 0; k < 8; ++k) {
        const bool is_open = children.at(k).is_open();
        toward_open += is_open ? offset(k) : -offset(k);
        open += is_open ? 1 : 0;
    }
    if (open == 0) {
        return Surface::solid();
    }
    if (open == 8) {
        return Surface::open();
    }
    if (glm::dot(toward_open, toward_open) == 0.0) {
        return open >= 4 ? Surface::open() : Surface::solid();
    }
    const DVec3 normal = glm::normalize(toward_open);
    double open_along = 0.0;
    double solid_along = 0.0;
    for (std::size_t k = 0; k < 8; ++k) {
        (children.at(k).is_open() ? open_along : solid_along) += glm::dot(normal, offset(k));
    }
    const double middle =
        ((open_along / open) + (solid_along / static_cast<double>(8 - open))) / 2.0;
    return Surface::plane(normal, -middle);
}

} // namespace

Surface Surface::plane(DVec3 normal, double distance, std::uint16_t material) noexcept {
    const double out = reach(normal);
    if (distance >= out) {
        return open();
    }
    if (distance <= -out) {
        return solid();
    }
    const double across = std::abs(normal.x) + std::abs(normal.y) + std::abs(normal.z);
    double u = normal.x / across;
    double v = normal.y / across;
    if (normal.z < 0.0) {
        const double folded_u = (1.0 - std::abs(v)) * sign(u);
        v = (1.0 - std::abs(u)) * sign(v);
        u = folded_u;
    }
    constexpr double most = std::numeric_limits<std::int32_t>::max() - 1.0;
    return Surface{static_cast<std::int32_t>(std::clamp(std::round(distance * scale), -most, most)),
                   flattened(u), flattened(v), material};
}

DVec3 Surface::normal() const noexcept {
    double x = ((u_ / full) * 2.0) - 1.0;
    double y = ((v_ / full) * 2.0) - 1.0;
    const double z = 1.0 - std::abs(x) - std::abs(y);
    const double fold = std::max(-z, 0.0);
    x += x >= 0.0 ? -fold : fold;
    y += y >= 0.0 ? -fold : fold;
    return glm::normalize(DVec3{x, y, z});
}

Result<Surface> Surface::from_raw(std::int32_t distance, std::uint16_t u, std::uint16_t v,
                                  std::uint16_t material) {
    const bool uniform = distance == std::numeric_limits<std::int32_t>::min() ||
                         distance == std::numeric_limits<std::int32_t>::max();
    if (uniform && (u != 0 || v != 0 || material != 0)) {
        return make_error(ErrorCode::Parse, "a surface missing its node with a normal or material");
    }
    return Surface{distance, u, v, material};
}

namespace {

// Blends follow the blendable entries, eight shares of each ordered pair, from half up.
constexpr int blend_shares = Blend::steps / 2;
constexpr int blends = Blend::blendable * Blend::blendable * blend_shares;

} // namespace

std::uint16_t Blend::entry() const noexcept {
    if (first >= blendable || second >= blendable || first == second) {
        return first;
    }
    const bool swap = share < 0.5;
    const auto parts = static_cast<int>(std::lround((swap ? 1.0 - share : share) * steps));
    if (parts >= steps) {
        return swap ? second : first;
    }
    const int over = swap ? second : first;
    const int under = swap ? first : second;
    return static_cast<std::uint16_t>(blendable + (((over * blendable) + under) * blend_shares) +
                                      (parts - blend_shares));
}

Blend Blend::of(std::uint16_t entry) noexcept {
    if (entry < blendable || entry >= blendable + blends) {
        return {.first = entry, .second = entry, .share = 1.0};
    }
    const int index = entry - blendable;
    const int pair = index / blend_shares;
    return {.first = static_cast<std::uint16_t>(pair / blendable),
            .second = static_cast<std::uint16_t>(pair % blendable),
            .share = static_cast<double>(blend_shares + (index % blend_shares)) / steps};
}

void Blending::add(std::uint16_t entry, double weight) noexcept {
    if (entry == 0) {
        return;
    }
    const Blend blend = Blend::of(entry);
    tally(blend.first, weight * blend.share);
    if (blend.second != blend.first) {
        tally(blend.second, weight * (1.0 - blend.share));
    }
}

void Blending::tally(std::uint16_t entry, double weight) noexcept {
    for (std::size_t k = 0; k < count_; ++k) {
        if (entries_.at(k) == entry) {
            weights_.at(k) += weight;
            return;
        }
    }
    if (count_ < entries_.size()) {
        entries_.at(count_) = entry;
        weights_.at(count_) = weight;
        ++count_;
    }
}

std::uint16_t Blending::entry() const noexcept {
    const auto before = [&](std::size_t a, std::size_t b) {
        return weights_.at(a) > weights_.at(b) ||
               (weights_.at(a) == weights_.at(b) && entries_.at(a) < entries_.at(b));
    };
    std::size_t best = count_;
    std::size_t next = count_;
    for (std::size_t k = 0; k < count_; ++k) {
        if (best == count_ || before(k, best)) {
            next = best;
            best = k;
        } else if (next == count_ || before(k, next)) {
            next = k;
        }
    }
    if (best == count_) {
        return 0;
    }
    if (next == count_) {
        return entries_.at(best);
    }
    const double both = weights_.at(best) + weights_.at(next);
    return Blend{.first = entries_.at(best),
                 .second = entries_.at(next),
                 .share = both > 0.0 ? weights_.at(best) / both : 1.0}
        .entry();
}

Surface Shape::summary(const std::array<Value, 8>& children) noexcept {
    DVec3 normals{0.0};
    double distances = 0.0;
    int crossing = 0;
    Blending shown;
    for (std::size_t k = 0; k < 8; ++k) {
        const Surface& child = children.at(k);
        if (child.crosses()) {
            shown.add(child.material());
            const DVec3 normal = child.normal();
            normals += normal;
            // A child's distance in its parent's sides, at its parent's middle.
            distances += (child.distance() / 2.0) - glm::dot(normal, offset(k));
            ++crossing;
        }
    }
    if (crossing == 0) {
        return between(children);
    }
    if (glm::dot(normals, normals) < 1e-12) {
        return between(children);
    }
    return Surface::plane(glm::normalize(normals), distances / crossing, shown.entry());
}

std::optional<Shape::Value> Shape::merged(const std::array<Value, 8>& children,
                                          double side) noexcept {
    if (std::ranges::all_of(children, [&](const Surface& child) {
            return !child.crosses() && child == children[0];
        })) {
        return children[0];
    }
    const Surface parent = summary(children);
    if (!parent.crosses()) {
        return std::nullopt;
    }
    const DVec3 normal = parent.normal();
    const double out = reach(normal);
    const double slack = tolerance / side;
    for (std::size_t k = 0; k < 8; ++k) {
        const Surface& child = children.at(k);
        // The parent's plane in the child's sides, at the child's middle.
        const double here = 2.0 * (parent.distance() + glm::dot(normal, offset(k)));
        if (child.is_open() && here < out - slack) {
            return std::nullopt;
        }
        if (child.is_solid() && here > -out + slack) {
            return std::nullopt;
        }
        if (child.crosses()) {
            const DVec3 tilt = glm::abs(normal - child.normal());
            const double apart =
                std::abs(here - child.distance()) + ((tilt.x + tilt.y + tilt.z) / 2.0);
            if (apart > slack) {
                return std::nullopt;
            }
        }
    }
    return parent;
}

std::uint64_t Shape::hash(const Value& value) noexcept {
    const std::uint64_t place =
        (std::uint64_t{std::bit_cast<std::uint32_t>(value.raw_distance())} << 32U) |
        (std::uint64_t{value.raw_u()} << 16U) | value.raw_v();
    return place ^ (value.material() * 0x9e37'79b9'7f4a'7c15ULL);
}

void Shape::write(ByteWriter& out, const Value& value) {
    out.write(std::bit_cast<std::uint32_t>(value.raw_distance()));
    out.write(value.raw_u());
    out.write(value.raw_v());
    out.write(value.material());
}

Result<Shape::Value> Shape::read(ByteReader& in) {
    const auto distance = in.read<std::uint32_t>();
    const auto u = in.read<std::uint16_t>();
    const auto v = in.read<std::uint16_t>();
    const auto material = in.read<std::uint16_t>();
    if (!distance || !u || !v || !material) {
        return make_error(ErrorCode::Parse, "a surface cut short");
    }
    return Surface::from_raw(std::bit_cast<std::int32_t>(*distance), *u, *v, *material);
}

namespace {

// Of the byte a stand is written with: ground, then which kinds have shares.
constexpr std::uint8_t ground_bit = 0x80;

} // namespace

Stand Stand::of(double cover, const std::array<double, kinds>& shares) noexcept {
    Stand stand{.ground = true,
                .cover =
                    static_cast<std::uint8_t>(std::lround(std::clamp(cover, 0.0, 1.0) * whole)),
                .shares = {}};
    double total = 0.0;
    for (const double share : shares) {
        total += std::max(share, 0.0);
    }
    if (total <= 0.0) {
        return stand;
    }
    // Each its floor, then the parts left over to those that lost most to it, the first of those
    // tied first.
    std::array<double, kinds> lost{};
    int given = 0;
    for (std::size_t kind = 0; kind < kinds; ++kind) {
        const double exact = std::max(shares.at(kind), 0.0) / total * whole;
        const double floor = std::floor(exact);
        stand.shares.at(kind) = static_cast<std::uint8_t>(floor);
        lost.at(kind) = exact - floor;
        given += static_cast<int>(floor);
    }
    for (; given < whole; ++given) {
        const auto most = static_cast<std::size_t>(std::ranges::max_element(lost) - lost.begin());
        ++stand.shares.at(most);
        lost.at(most) = -1.0;
    }
    return stand;
}

Stand Woods::summary(const std::array<Value, 8>& children) noexcept {
    int grounds = 0;
    double cover = 0.0;
    std::array<double, Stand::kinds> shares{};
    std::array<double, Stand::kinds> even{};
    for (const Value& child : children) {
        if (!child.ground) {
            continue;
        }
        ++grounds;
        cover += child.cover;
        for (std::size_t kind = 0; kind < Stand::kinds; ++kind) {
            shares.at(kind) += static_cast<double>(child.cover) * child.shares.at(kind);
            even.at(kind) += child.shares.at(kind);
        }
    }
    if (grounds == 0) {
        return {};
    }
    // Where none is under crowns, what kinds there'd be stand evenly for them.
    return Stand::of(cover / (grounds * static_cast<double>(Stand::whole)),
                     cover > 0.0 ? shares : even);
}

std::optional<Stand> Woods::merged(const std::array<Value, 8>& children, double /*side*/) noexcept {
    const Value whole = summary(children);
    for (const Value& child : children) {
        if (!child.ground) {
            continue;
        }
        if (std::abs(child.cover - whole.cover) > tolerance) {
            return std::nullopt;
        }
        for (std::size_t kind = 0; kind < Stand::kinds; ++kind) {
            if (std::abs(child.shares.at(kind) - whole.shares.at(kind)) > tolerance) {
                return std::nullopt;
            }
        }
    }
    return whole;
}

std::uint64_t Woods::hash(const Value& value) noexcept {
    std::uint64_t packed = (value.ground ? 1U : 0U) | (std::uint64_t{value.cover} << 8U);
    for (std::size_t kind = 0; kind < Stand::kinds; ++kind) {
        packed |= std::uint64_t{value.shares.at(kind)} << (16U + (8U * kind));
    }
    return packed * 0x9e37'79b9'7f4a'7c15ULL;
}

void Woods::write(ByteWriter& out, const Value& value) {
    if (!value.ground) {
        out.write(std::uint8_t{0});
        return;
    }
    unsigned held = ground_bit;
    for (std::size_t kind = 0; kind < Stand::kinds; ++kind) {
        held |= value.shares.at(kind) != 0 ? 1U << kind : 0U;
    }
    out.write(static_cast<std::uint8_t>(held));
    out.write(value.cover);
    for (const std::uint8_t share : value.shares) {
        if (share != 0) {
            out.write(share);
        }
    }
}

Result<Stand> Woods::read(ByteReader& in) {
    const auto held = in.read<std::uint8_t>();
    if (!held) {
        return make_error(ErrorCode::Parse, "a stand cut short");
    }
    Stand stand;
    if (*held == 0) {
        return stand;
    }
    if ((*held & ground_bit) == 0 || (*held & ~(ground_bit | ((1U << Stand::kinds) - 1U))) != 0) {
        return make_error(ErrorCode::Parse, "a stand neither ground nor none");
    }
    stand.ground = true;
    const auto cover = in.read<std::uint8_t>();
    if (!cover) {
        return make_error(ErrorCode::Parse, "a stand cut short");
    }
    stand.cover = *cover;
    int total = 0;
    for (std::size_t kind = 0; kind < Stand::kinds; ++kind) {
        if ((*held & (1U << kind)) == 0) {
            continue;
        }
        const auto read = in.read<std::uint8_t>();
        if (!read) {
            return make_error(ErrorCode::Parse, "a stand cut short");
        }
        if (*read == 0) {
            return make_error(ErrorCode::Parse, "a stand's kind held with no share");
        }
        stand.shares.at(kind) = *read;
        total += *read;
    }
    if (total != 0 && total != Stand::whole) {
        return make_error(ErrorCode::Parse, "a stand's shares not summing to the whole");
    }
    return stand;
}

} // namespace hog::grid
