#include <hog/globe/brick.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

namespace hog::globe {
namespace {

// Bits a stored value takes.
template <typename T>
using Raw =
    std::make_unsigned_t<typename std::conditional_t<std::is_enum_v<T>, std::underlying_type<T>,
                                                     std::type_identity<T>>::type>;

template <typename T>
Raw<T> raw_of(T value) noexcept {
    if constexpr (std::is_enum_v<T>) {
        return static_cast<Raw<T>>(std::to_underlying(value));
    } else {
        return std::bit_cast<Raw<T>>(value);
    }
}

template <typename T>
Result<T> value_of(Raw<T> raw) {
    if constexpr (std::is_same_v<T, Material>) {
        if (raw >= material_count) {
            return make_error(ErrorCode::Parse, std::format("no material {}", raw));
        }
        return static_cast<Material>(raw);
    } else {
        return std::bit_cast<T>(raw);
    }
}

template <typename T>
void write_value(ByteWriter& out, T value) {
    out.write(raw_of(value));
}

template <typename T>
Result<T> read_value(ByteReader& in) {
    const auto raw = in.read<Raw<T>>();
    if (!raw) {
        return std::unexpected(raw.error());
    }
    return value_of<T>(*raw);
}

// How a channel is written: one value, every value, or palette indices this many bits wide.
constexpr std::uint8_t uniform_mark = 0;
constexpr std::uint8_t full_mark = 0xFF;

// Indices never straddle a word, so they're as wide as divides 64.
constexpr std::uint8_t index_bits(std::size_t palette) noexcept {
    std::uint8_t bits = 1;
    while ((std::size_t{1} << bits) < palette) {
        bits = static_cast<std::uint8_t>(bits * 2);
    }
    return bits;
}

constexpr std::size_t word_count(std::uint8_t bits) noexcept {
    return static_cast<std::size_t>(brick_voxels) * bits / 64;
}

// Palette entries, then indices `bits` wide, into `values`.
template <typename T>
Result<void> read_palette(ByteReader& in, std::uint8_t bits, std::span<T, brick_voxels> values) {
    if (bits > 16 || !std::has_single_bit(bits)) {
        return make_error(ErrorCode::Parse, std::format("indices can't be {} bits", bits));
    }
    const auto size = in.read_varint();
    if (!size) {
        return std::unexpected(size.error());
    }
    if (*size < 2 || *size > (std::uint64_t{1} << bits) || *size > brick_voxels) {
        return make_error(ErrorCode::Parse,
                          std::format("a palette of {} for {} bits", *size, bits));
    }
    std::vector<T> palette;
    palette.reserve(*size);
    for (std::uint64_t entry = 0; entry < *size; ++entry) {
        const auto value = read_value<T>(in);
        if (!value) {
            return std::unexpected(value.error());
        }
        palette.push_back(*value);
    }
    const std::size_t per_word = 64 / bits;
    const std::uint64_t mask = (std::uint64_t{1} << bits) - 1;
    for (std::size_t word = 0; word < word_count(bits); ++word) {
        const auto read = in.read<std::uint64_t>();
        if (!read) {
            return std::unexpected(read.error());
        }
        for (std::size_t k = 0; k < per_word; ++k) {
            const std::uint64_t entry = (*read >> (k * bits)) & mask;
            if (entry >= palette.size()) {
                return make_error(ErrorCode::Parse, "a palette index past its end");
            }
            values[(word * per_word) + k] = palette[entry];
        }
    }
    return {};
}

} // namespace

template <typename T>
Packed<T> Packed<T>::pack(std::span<const T, brick_voxels> values) {
    std::array<T, brick_voxels> sorted{};
    std::ranges::copy(values, sorted.begin());
    std::ranges::sort(sorted);
    const auto distinct = static_cast<std::size_t>(
        std::distance(sorted.begin(), std::ranges::unique(sorted).begin()));
    Packed packed;
    if (distinct == 1) {
        packed.one_ = sorted[0];
        return packed;
    }
    const std::uint8_t bits = index_bits(distinct);
    if ((distinct * sizeof(T)) + (word_count(bits) * sizeof(std::uint64_t)) >=
        brick_voxels * sizeof(T)) {
        packed.values_.assign(values.begin(), values.end());
        return packed;
    }
    // The palette in order, so the same voxels always pack the same.
    packed.values_.assign(sorted.begin(),
                          std::next(sorted.begin(), static_cast<std::ptrdiff_t>(distinct)));
    packed.bits_ = bits;
    packed.words_.assign(word_count(bits), 0);
    const std::size_t per_word = 64 / bits;
    for (std::size_t index = 0; index < values.size(); ++index) {
        const auto entry = static_cast<std::uint64_t>(std::distance(
            packed.values_.begin(), std::ranges::lower_bound(packed.values_, values[index])));
        packed.words_[index / per_word] |= entry << ((index % per_word) * bits);
    }
    return packed;
}

template <typename T>
T Packed<T>::at(int index) const noexcept {
    if (values_.empty()) {
        return one_;
    }
    const auto at = static_cast<std::size_t>(index);
    if (words_.empty()) {
        return values_[at];
    }
    const std::size_t per_word = 64 / bits_;
    const std::uint64_t mask = (std::uint64_t{1} << bits_) - 1;
    return values_[(words_[at / per_word] >> ((at % per_word) * bits_)) & mask];
}

template <typename T>
void Packed<T>::unpack(std::span<T, brick_voxels> into) const noexcept {
    for (int index = 0; index < brick_voxels; ++index) {
        into[static_cast<std::size_t>(index)] = at(index);
    }
}

template <typename T>
std::size_t Packed<T>::bytes() const noexcept {
    return (values_.capacity() * sizeof(T)) + (words_.capacity() * sizeof(std::uint64_t));
}

template <typename T>
void Packed<T>::write(ByteWriter& out) const {
    if (values_.empty()) {
        out.write(uniform_mark);
        write_value(out, one_);
        return;
    }
    if (words_.empty()) {
        out.write(full_mark);
        for (const T value : values_) {
            write_value(out, value);
        }
        return;
    }
    out.write(bits_);
    out.write_varint(values_.size());
    for (const T value : values_) {
        write_value(out, value);
    }
    for (const std::uint64_t word : words_) {
        out.write(word);
    }
}

template <typename T>
Result<Packed<T>> Packed<T>::read(ByteReader& in) {
    const auto mark = in.read<std::uint8_t>();
    if (!mark) {
        return std::unexpected(mark.error());
    }
    std::array<T, brick_voxels> values{};
    if (*mark == uniform_mark) {
        const auto value = read_value<T>(in);
        if (!value) {
            return std::unexpected(value.error());
        }
        values.fill(*value);
    } else if (*mark == full_mark) {
        for (T& value : values) {
            const auto read = read_value<T>(in);
            if (!read) {
                return std::unexpected(read.error());
            }
            value = *read;
        }
    } else if (auto read = read_palette<T>(in, *mark, values); !read) {
        return std::unexpected(read.error());
    }
    // Packed afresh, so what was read is held just as the same voxels would be.
    return pack(values);
}

template class Packed<std::int16_t>;
template class Packed<std::uint8_t>;
template class Packed<std::uint16_t>;
template class Packed<Material>;

Brick Brick::pack(std::span<const Voxel, brick_voxels> voxels) {
    if (std::ranges::all_of(voxels, [&](const Voxel& voxel) { return voxel == voxels[0]; })) {
        return Brick{voxels[0]};
    }
    const auto channel = [&]<typename T>(T Voxel::* member) {
        std::array<T, brick_voxels> values{};
        std::ranges::transform(voxels, values.begin(), member);
        return Packed<T>::pack(values);
    };
    Brick brick;
    brick.parts_ = std::make_unique<Parts>(Parts{
        .solid = channel(&Voxel::solid),
        .condition = channel(&Voxel::condition),
        .grain = channel(&Voxel::grain),
        .grain_amount = channel(&Voxel::grain_amount),
        .liquid = channel(&Voxel::liquid),
        .liquid_amount = channel(&Voxel::liquid_amount),
        .temperature = channel(&Voxel::temperature),
        .solid_amount = channel(&Voxel::solid_amount),
    });
    return brick;
}

Brick::Brick(const Brick& other)
    : whole_{other.whole_},
      parts_{other.parts_ != nullptr ? std::make_unique<Parts>(*other.parts_) : nullptr} {}

Brick& Brick::operator=(const Brick& other) {
    if (this != &other) {
        Brick copy{other};
        *this = std::move(copy);
    }
    return *this;
}

Voxel Brick::at(int index) const noexcept {
    if (parts_ == nullptr) {
        return whole_;
    }
    return {.solid = parts_->solid.at(index),
            .condition = parts_->condition.at(index),
            .grain = parts_->grain.at(index),
            .grain_amount = parts_->grain_amount.at(index),
            .liquid = parts_->liquid.at(index),
            .liquid_amount = parts_->liquid_amount.at(index),
            .temperature = parts_->temperature.at(index),
            .solid_amount = parts_->solid_amount.at(index)};
}

void Brick::unpack(std::span<Voxel, brick_voxels> into) const noexcept {
    for (int index = 0; index < brick_voxels; ++index) {
        into[static_cast<std::size_t>(index)] = at(index);
    }
}

std::size_t Brick::bytes() const noexcept {
    if (parts_ == nullptr) {
        return sizeof(Brick);
    }
    return sizeof(Brick) + sizeof(Parts) + parts_->solid.bytes() + parts_->condition.bytes() + parts_->grain.bytes() +
           parts_->grain_amount.bytes() + parts_->liquid.bytes() + parts_->liquid_amount.bytes() +
           parts_->temperature.bytes() + parts_->solid_amount.bytes();
}

namespace {

// Bricks kept a distance and a normal for each voxel once; those are read past.
constexpr std::uint8_t old_whole_brick = 0;
constexpr std::uint8_t old_parted_brick = 1;
constexpr std::uint8_t whole_brick = 2;
constexpr std::uint8_t parted_brick = 3;

} // namespace

void Brick::write(ByteWriter& out) const {
    if (parts_ == nullptr) {
        out.write(whole_brick);
        write_value(out, whole_.solid);
        write_value(out, whole_.condition);
        write_value(out, whole_.grain);
        write_value(out, whole_.grain_amount);
        write_value(out, whole_.liquid);
        write_value(out, whole_.liquid_amount);
        write_value(out, whole_.temperature);
        write_value(out, whole_.solid_amount);
        return;
    }
    out.write(parted_brick);
    parts_->solid.write(out);
    parts_->condition.write(out);
    parts_->grain.write(out);
    parts_->grain_amount.write(out);
    parts_->liquid.write(out);
    parts_->liquid_amount.write(out);
    parts_->temperature.write(out);
    parts_->solid_amount.write(out);
}

namespace {

// Reads each field in turn, stopping at the first that fails.
class FieldReader {
public:
    explicit FieldReader(ByteReader& in) noexcept : in_{&in} {}

    template <typename T>
    T value() {
        return take(error_ ? Result<T>{} : read_value<T>(*in_));
    }

    template <typename T>
    Packed<T> packed() {
        return take(error_ ? Result<Packed<T>>{} : Packed<T>::read(*in_));
    }

    [[nodiscard]] const std::optional<Error>& error() const noexcept { return error_; }

private:
    template <typename T>
    T take(Result<T> read) {
        if (!read) {
            error_ = std::move(read.error());
            return T{};
        }
        return std::move(*read);
    }

    ByteReader* in_;
    std::optional<Error> error_;
};

} // namespace

Result<Brick> Brick::read(ByteReader& in) {
    const auto kind = in.read<std::uint8_t>();
    if (!kind) {
        return std::unexpected(kind.error());
    }
    FieldReader field{in};
    if (*kind == old_whole_brick) {
        static_cast<void>(field.value<std::int16_t>());
        static_cast<void>(field.value<std::uint16_t>());
    } else if (*kind == old_parted_brick) {
        static_cast<void>(field.packed<std::int16_t>());
        static_cast<void>(field.packed<std::uint16_t>());
    }
    if (*kind == whole_brick || *kind == old_whole_brick) {
        const Voxel voxel{.solid = field.value<Material>(),
                          .condition = field.value<std::uint8_t>(),
                          .grain = field.value<Material>(),
                          .grain_amount = field.value<std::uint16_t>(),
                          .liquid = field.value<Material>(),
                          .liquid_amount = field.value<std::uint16_t>(),
                          .temperature = field.value<std::uint16_t>(),
                          .solid_amount = field.value<std::uint16_t>()};
        if (field.error()) {
            return std::unexpected(*field.error());
        }
        return Brick{voxel};
    }
    if (*kind != parted_brick && *kind != old_parted_brick) {
        return make_error(ErrorCode::Parse, std::format("no brick kind {}", *kind));
    }
    Parts parts{.solid = field.packed<Material>(),
                .condition = field.packed<std::uint8_t>(),
                .grain = field.packed<Material>(),
                .grain_amount = field.packed<std::uint16_t>(),
                .liquid = field.packed<Material>(),
                .liquid_amount = field.packed<std::uint16_t>(),
                .temperature = field.packed<std::uint16_t>(),
                .solid_amount = field.packed<std::uint16_t>()};
    if (field.error()) {
        return std::unexpected(*field.error());
    }
    Brick brick;
    brick.parts_ = std::make_unique<Parts>(std::move(parts));
    // Every quantity alike throughout is one voxel throughout, held as such.
    if (std::ranges::all_of(
            std::array{brick.parts_->solid.is_uniform(), brick.parts_->condition.is_uniform(),
                       brick.parts_->grain.is_uniform(), brick.parts_->grain_amount.is_uniform(),
                       brick.parts_->liquid.is_uniform(), brick.parts_->liquid_amount.is_uniform(),
                       brick.parts_->temperature.is_uniform(),
                       brick.parts_->solid_amount.is_uniform()},
            std::identity{})) {
        return Brick{brick.at(0)};
    }
    return brick;
}

bool operator==(const Brick& a, const Brick& b) noexcept {
    if (a.parts_ == nullptr || b.parts_ == nullptr) {
        return a.parts_ == nullptr && b.parts_ == nullptr && a.whole_ == b.whole_;
    }
    return *a.parts_ == *b.parts_;
}

} // namespace hog::globe
