#include <hog/core/bytes.hpp>
#include <hog/grid/edits.hpp>

#include <format>
#include <ranges>
#include <type_traits>
#include <utility>

namespace hog::grid {

namespace {

constexpr std::uint8_t has_shape = 1;
constexpr std::uint8_t has_material = 2;
constexpr std::uint8_t has_water = 4;

template <typename Kind>
const std::optional<typename Kind::Value>& left_of(const Edit::Left& left) {
    if constexpr (std::is_same_v<Kind, Shape>) {
        return left.shape;
    } else if constexpr (std::is_same_v<Kind, Material>) {
        return left.material;
    } else {
        return left.water;
    }
}

template <typename Kind>
const std::optional<Held<Kind>>& held_of(const Page& page) {
    if constexpr (std::is_same_v<Kind, Shape>) {
        return page.shape;
    } else if constexpr (std::is_same_v<Kind, Material>) {
        return page.material;
    } else {
        return page.water;
    }
}

template <typename Kind>
Result<std::optional<typename Kind::Value>> read_value(ByteReader& in, bool present,
                                                       std::uint8_t version) {
    if (!present) {
        return std::nullopt;
    }
    auto value = Kind::read(in, version);
    if (!value) {
        return std::unexpected(std::move(value.error()));
    }
    return *value;
}

// Its values as their kinds wrote them in `shapes` of the shape format.
Result<Edit::Left> read_left(ByteReader& in, std::uint8_t shapes) {
    const auto depth = in.read<std::uint8_t>();
    const auto x = in.read_varint();
    const auto y = in.read_varint();
    const auto z = in.read_varint();
    const auto kinds = in.read<std::uint8_t>();
    if (!depth || !x || !y || !z || !kinds) {
        return make_error(ErrorCode::Parse, "an edit's node cut short");
    }
    if (*depth > max_depth || *x >> *depth != 0 || *y >> *depth != 0 || *z >> *depth != 0) {
        return make_error(ErrorCode::Parse, "an edit's node outside the grid");
    }
    if ((*kinds & ~(has_shape | has_material | has_water)) != 0) {
        return make_error(ErrorCode::Parse, std::format("an edit's kinds {}", *kinds));
    }
    auto shape = read_value<Shape>(in, (*kinds & has_shape) != 0, shapes);
    auto material = read_value<Material>(in, (*kinds & has_material) != 0, Material::format);
    auto water = read_value<Water>(in, (*kinds & has_water) != 0, Water::format);
    if (!shape || !material || !water) {
        return make_error(ErrorCode::Parse, "an edit's values cut short");
    }
    return Edit::Left{.key = Key{.depth = *depth,
                                 .x = static_cast<std::uint32_t>(*x),
                                 .y = static_cast<std::uint32_t>(*y),
                                 .z = static_cast<std::uint32_t>(*z)},
                      .shape = *shape,
                      .material = *material,
                      .water = *water};
}

Result<Edit> read_edit(ByteReader& in, std::uint8_t shapes) {
    const auto id = in.read_varint();
    const auto author = in.read_varint();
    const auto time = in.read_signed();
    const auto generator = in.read<std::uint32_t>();
    const auto count = in.read_varint();
    if (!id || !author || !time || !generator || !count) {
        return make_error(ErrorCode::Parse, "an edit cut short");
    }
    // Each node is at least a depth, three coordinates and its kinds.
    if (*count > in.remaining() / 5) {
        return make_error(ErrorCode::Parse, "an edit's node count past its bytes");
    }
    Edit edit{.id = *id, .author = *author, .time = *time, .generator = *generator, .left = {}};
    edit.left.reserve(*count);
    for (std::uint64_t n = 0; n < *count; ++n) {
        auto left = read_left(in, shapes);
        if (!left) {
            return std::unexpected(std::move(left.error()));
        }
        edit.left.push_back(*left);
    }
    return edit;
}

} // namespace

template <typename Kind>
std::optional<typename Kind::Value> EditLog::read(const Page& page, Key key) const {
    for (const Edit& edit : edits | std::views::reverse) {
        for (const Edit::Left& left : edit.left | std::views::reverse) {
            const auto& value = left_of<Kind>(left);
            if (value && left.key.contains(key)) {
                return value;
            }
        }
    }
    const auto& held = held_of<Kind>(page);
    if (!held || !page.root.contains(key) || key.depth - page.root.depth > held->tree.levels()) {
        return std::nullopt;
    }
    const auto down = static_cast<std::uint8_t>(key.depth - page.root.depth);
    return held->tree
        .at(down, key.x - (page.root.x << down), key.y - (page.root.y << down),
            key.z - (page.root.z << down))
        .value;
}

std::vector<std::byte> EditLog::encode() const {
    ByteWriter out;
    out.write(format);
    out.write_varint(edits.size());
    for (const Edit& edit : edits) {
        out.write_varint(edit.id);
        out.write_varint(edit.author);
        out.write_signed(edit.time);
        out.write(edit.generator);
        out.write_varint(edit.left.size());
        for (const Edit::Left& left : edit.left) {
            out.write(left.key.depth);
            out.write_varint(left.key.x);
            out.write_varint(left.key.y);
            out.write_varint(left.key.z);
            out.write(static_cast<std::uint8_t>((left.shape ? has_shape : 0U) |
                                                (left.material ? has_material : 0U) |
                                                (left.water ? has_water : 0U)));
            if (left.shape) {
                Shape::write(out, *left.shape);
            }
            if (left.material) {
                Material::write(out, *left.material);
            }
            if (left.water) {
                Water::write(out, *left.water);
            }
        }
    }
    return std::move(out).take();
}

Result<EditLog> EditLog::decode(std::span<const std::byte> bytes) {
    ByteReader in{bytes};
    const auto version = in.read<std::uint8_t>();
    const auto count = in.read_varint();
    if (!version || !count) {
        return make_error(ErrorCode::Parse, "an edit log cut short");
    }
    if (*version == 0 || *version > format) {
        return make_error(ErrorCode::Unsupported, std::format("edit log format {}", *version));
    }
    if (*count > in.remaining()) {
        return make_error(ErrorCode::Parse, "an edit count past its bytes");
    }
    EditLog log;
    log.edits.reserve(*count);
    for (std::uint64_t n = 0; n < *count; ++n) {
        // Shapes gained their roughness in the second.
        auto edit = read_edit(in, *version == 1 ? std::uint8_t{1} : Shape::format);
        if (!edit) {
            return std::unexpected(std::move(edit.error()));
        }
        log.edits.push_back(std::move(*edit));
    }
    if (auto done = in.finish(); !done) {
        return std::unexpected(std::move(done.error()));
    }
    return log;
}

template std::optional<Shape::Value> EditLog::read<Shape>(const Page&, Key) const;
template std::optional<Material::Value> EditLog::read<Material>(const Page&, Key) const;
template std::optional<Water::Value> EditLog::read<Water>(const Page&, Key) const;

} // namespace hog::grid
