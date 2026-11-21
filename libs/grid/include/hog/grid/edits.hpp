#pragma once

#include <hog/core/result.hpp>
#include <hog/grid/key.hpp>
#include <hog/grid/page.hpp>
#include <hog/grid/tree.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace hog::grid {

// One change made to the world: which, who made it, when (milliseconds since 1970), the
// generator version it was made against, and what it left: each node it set and what each kind
// holds there after, all through it.
struct Edit {
    struct Left {
        Key key;
        std::optional<Shape::Value> shape;
        std::optional<Material::Value> material;
        std::optional<Water::Value> water;

        friend bool operator==(const Left&, const Left&) = default;
    };

    std::uint64_t id = 0;
    std::uint64_t author = 0;
    std::int64_t time = 0;
    std::uint32_t generator = 0;
    std::vector<Left> left;

    friend bool operator==(const Edit&, const Edit&) = default;
};

// Edits over the baked trees, oldest first.
struct EditLog {
    // 2 wrote shapes with their roughness; 1 still reads.
    static constexpr std::uint8_t format = 2;

    std::vector<Edit> edits;

    // What `kind` holds at `key`: what the latest edit left over it, else the page's tree; none
    // where neither says, as where the page lacks that tree or the key lies outside the page.
    template <typename Kind>
    [[nodiscard]] std::optional<typename Kind::Value> read(const Page& page, Key key) const;

    // Little-endian: format, a varint count, then each edit: id, author and zigzagged time as
    // varints, the generator version, and a varint count of what it left, each a key (depth, then
    // x, y, z as varints), a byte saying which kinds follow (1 shape, 2 material, 4 water) and
    // their values.
    [[nodiscard]] std::vector<std::byte> encode() const;
    // Fails, saying why, on bytes that aren't a log or a key outside the grid.
    [[nodiscard]] static Result<EditLog> decode(std::span<const std::byte> bytes);

    friend bool operator==(const EditLog&, const EditLog&) = default;
};

extern template std::optional<Shape::Value> EditLog::read<Shape>(const Page&, Key) const;
extern template std::optional<Material::Value> EditLog::read<Material>(const Page&, Key) const;
extern template std::optional<Water::Value> EditLog::read<Water>(const Page&, Key) const;

} // namespace hog::grid
