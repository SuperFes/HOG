#include <hog/baked/node.hpp>

namespace hog::baked {
namespace {

constexpr std::uint8_t largest_size = 0xFF;

// Face, then the Morton code of (x, y, z) a bit of each at a time from the top, z first, then
// `size`.
Key key_from(std::uint8_t face, std::uint32_t x, std::uint32_t y, std::uint32_t z,
             std::uint8_t size) noexcept {
    Key key{};
    key[0] = std::byte{face};
    std::size_t bit = 0;
    for (int b = coordinate_bits - 1; b >= 0; --b) {
        for (const std::uint32_t v : {z, y, x}) {
            if (((v >> static_cast<unsigned>(b)) & 1U) != 0) {
                key.at(1 + (bit / 8)) |= std::byte{static_cast<std::uint8_t>(0x80U >> (bit % 8))};
            }
            ++bit;
        }
    }
    key[key_bytes - 1] = std::byte{size};
    return key;
}

} // namespace

Key key_of(const Node& node) noexcept {
    return key_from(node.face, node.x, node.y, node.z, node.size);
}

Key key_at(Point p) noexcept {
    return key_from(p.face, p.x, p.y, p.z, largest_size);
}

Key last_key_in(const Node& node) noexcept {
    const auto last = static_cast<std::uint32_t>(node.side() - 1U);
    return key_from(node.face, node.x + last, node.y + last, node.z + last, largest_size);
}

std::optional<Node> node_of(std::span<const std::byte> key) noexcept {
    if (key.size() != key_bytes) {
        return std::nullopt;
    }
    Node node{.face = std::to_integer<std::uint8_t>(key[0]),
              .size = std::to_integer<std::uint8_t>(key[key_bytes - 1])};
    std::size_t bit = 0;
    for (int b = coordinate_bits - 1; b >= 0; --b) {
        for (std::uint32_t* v : {&node.z, &node.y, &node.x}) {
            const auto byte = std::to_integer<unsigned>(key[1 + (bit / 8)]);
            if ((byte & (0x80U >> (bit % 8))) != 0) {
                *v |= std::uint32_t{1} << static_cast<unsigned>(b);
            }
            ++bit;
        }
    }
    const std::uint32_t low = (std::uint32_t{1} << std::min<unsigned>(node.size, 31U)) - 1U;
    if (node.face >= 6 || node.size > coordinate_bits || ((node.x | node.y | node.z) & low) != 0) {
        return std::nullopt;
    }
    return node;
}

} // namespace hog::baked
