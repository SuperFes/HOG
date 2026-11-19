#include <hog/core/bytes.hpp>
#include <hog/grid/piece.hpp>

#include <format>
#include <utility>

namespace hog::grid {

namespace {

constexpr std::uint8_t has_shape = 1;
constexpr std::uint8_t has_material = 2;
constexpr std::uint8_t has_water = 4;
constexpr std::uint8_t has_woods = 8;

template <typename Kind>
void write(ByteWriter& out, const std::optional<Tree<Kind>>& tree) {
    if (tree) {
        const std::vector<std::byte> bytes = tree->encode();
        out.write_varint(bytes.size());
        out.write_bytes(bytes);
    }
}

template <typename Kind>
Result<std::optional<Tree<Kind>>> read(ByteReader& in, bool present, Key root) {
    if (!present) {
        return std::nullopt;
    }
    const auto length = in.read_varint();
    if (!length) {
        return make_error(ErrorCode::Parse, "a piece's tree cut short");
    }
    const auto bytes = in.read_bytes(*length);
    if (!bytes) {
        return std::unexpected(bytes.error());
    }
    auto tree = Tree<Kind>::decode(*bytes);
    if (!tree) {
        return std::unexpected(std::move(tree.error()));
    }
    if (tree->levels() > max_depth - root.depth) {
        return make_error(ErrorCode::Parse, "a piece's tree deeper than the grid");
    }
    return std::optional<Tree<Kind>>{std::move(*tree)};
}

} // namespace

std::vector<std::byte> Piece::encode() const {
    ByteWriter out;
    out.write(format);
    out.write(root.depth);
    out.write(root.x);
    out.write(root.y);
    out.write(root.z);
    out.write(generator);
    out.write(static_cast<std::uint8_t>((shape ? has_shape : 0U) | (material ? has_material : 0U) |
                                        (water ? has_water : 0U) | (woods ? has_woods : 0U)));
    write(out, shape);
    write(out, material);
    write(out, water);
    write(out, woods);
    return std::move(out).take();
}

Result<Piece> Piece::decode(std::span<const std::byte> bytes) {
    ByteReader in{bytes};
    const auto version = in.read<std::uint8_t>();
    const auto depth = in.read<std::uint8_t>();
    const auto x = in.read<std::uint32_t>();
    const auto y = in.read<std::uint32_t>();
    const auto z = in.read<std::uint32_t>();
    const auto generator = in.read<std::uint32_t>();
    const auto trees = in.read<std::uint8_t>();
    if (!version || !depth || !x || !y || !z || !generator || !trees) {
        return make_error(ErrorCode::Parse, "a piece cut short");
    }
    if (*version != format) {
        return make_error(ErrorCode::Unsupported, std::format("piece format {}", *version));
    }
    const Key root{.depth = *depth, .x = *x, .y = *y, .z = *z};
    if (!root.is_valid()) {
        return make_error(ErrorCode::Parse, "a piece rooted outside the grid");
    }
    if ((*trees & ~(has_shape | has_material | has_water | has_woods)) != 0) {
        return make_error(ErrorCode::Parse, std::format("piece trees {}", *trees));
    }
    auto shape = read<Shape>(in, (*trees & has_shape) != 0, root);
    if (!shape) {
        return std::unexpected(std::move(shape.error()));
    }
    auto material = read<Material>(in, (*trees & has_material) != 0, root);
    if (!material) {
        return std::unexpected(std::move(material.error()));
    }
    auto water = read<Water>(in, (*trees & has_water) != 0, root);
    if (!water) {
        return std::unexpected(std::move(water.error()));
    }
    auto woods = read<Woods>(in, (*trees & has_woods) != 0, root);
    if (!woods) {
        return std::unexpected(std::move(woods.error()));
    }
    if (auto done = in.finish(); !done) {
        return std::unexpected(std::move(done.error()));
    }
    return Piece{.root = root,
                 .generator = *generator,
                 .shape = std::move(*shape),
                 .material = std::move(*material),
                 .water = std::move(*water),
                 .woods = std::move(*woods)};
}

} // namespace hog::grid
