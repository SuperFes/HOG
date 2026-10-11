#include <hog/core/bytes.hpp>
#include <hog/planet/change.hpp>

#include <format>
#include <type_traits>
#include <utility>

namespace hog::planet {

namespace {

static_assert(kind_of(SetElevation{}) == ChangeKind::SetElevation);
static_assert(kind_of(Excavate{}) == ChangeKind::Excavate);
static_assert(kind_of(SetBiome{}) == ChangeKind::SetBiome);
static_assert(change_kind_count == static_cast<int>(ChangeKind::SetBiome) + 1);

template <typename Payload>
[[nodiscard]] Result<ChangePayload> finish(const ByteReader& reader, Payload payload) {
    if (const auto done = reader.finish(); !done) {
        return make_error(ErrorCode::Parse,
                          std::format("change payload: {}", done.error().message));
    }
    return ChangePayload{payload};
}

} // namespace

std::string_view to_string(Layer layer) noexcept {
    switch (layer) {
    case Layer::Authored:
        return "authored";
    case Layer::Simulation:
        return "simulation";
    case Layer::Player:
        return "player";
    }
    return "unknown";
}

std::string_view to_string(ChangeKind kind) noexcept {
    switch (kind) {
    case ChangeKind::SetElevation:
        return "set elevation";
    case ChangeKind::Excavate:
        return "excavate";
    case ChangeKind::SetBiome:
        return "set biome";
    }
    return "unknown";
}

bool ChangeFilter::matches(const Change& change) const noexcept {
    return (!author || change.author == *author) && (!layer || change.layer == *layer) &&
           change.time >= from && change.time < until;
}

std::vector<std::byte> encode_payload(const ChangePayload& payload) {
    ByteWriter writer;
    std::visit(
        [&](const auto& change) {
            using Payload = std::decay_t<decltype(change)>;
            if constexpr (std::is_same_v<Payload, SetElevation>) {
                writer.write(static_cast<std::uint64_t>(change.elevation));
            } else if constexpr (std::is_same_v<Payload, Excavate>) {
                writer.write(static_cast<std::uint64_t>(change.depth));
            } else {
                static_assert(std::is_same_v<Payload, SetBiome>);
                writer.write(std::to_underlying(change.biome));
            }
        },
        payload);
    return std::move(writer).take();
}

Result<ChangePayload> decode_payload(std::uint8_t kind, std::span<const std::byte> bytes) {
    ByteReader reader{bytes};
    switch (static_cast<ChangeKind>(kind)) {
    case ChangeKind::SetElevation: {
        const auto elevation = reader.read<std::uint64_t>();
        if (!elevation) {
            return std::unexpected(elevation.error());
        }
        return finish(reader, SetElevation{static_cast<std::int64_t>(*elevation)});
    }
    case ChangeKind::Excavate: {
        const auto depth = reader.read<std::uint64_t>();
        if (!depth) {
            return std::unexpected(depth.error());
        }
        return finish(reader, Excavate{static_cast<std::int64_t>(*depth)});
    }
    case ChangeKind::SetBiome: {
        const auto biome = reader.read<std::uint8_t>();
        if (!biome) {
            return std::unexpected(biome.error());
        }
        if (*biome >= static_cast<std::uint64_t>(biome_count)) {
            return make_error(ErrorCode::Parse, std::format("unknown biome {}", *biome));
        }
        return finish(reader, SetBiome{static_cast<Biome>(*biome)});
    }
    }
    return make_error(ErrorCode::Parse, std::format("unknown change kind {}", kind));
}

} // namespace hog::planet
