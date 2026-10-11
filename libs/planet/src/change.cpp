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

class Writer {
public:
    void write(std::uint64_t value, int bytes) {
        for (int byte = 0; byte < bytes; ++byte) {
            out_.push_back(static_cast<std::byte>(value >> (8 * byte)));
        }
    }

    [[nodiscard]] std::vector<std::byte> take() && { return std::move(out_); }

private:
    std::vector<std::byte> out_;
};

class Reader {
public:
    explicit Reader(std::span<const std::byte> bytes) : bytes_{bytes} {}

    [[nodiscard]] Result<std::uint64_t> read(int bytes) {
        if (bytes_.size() < static_cast<std::size_t>(bytes)) {
            return make_error(ErrorCode::Parse, "change payload is truncated");
        }
        std::uint64_t value = 0;
        for (int byte = 0; byte < bytes; ++byte) {
            value |= std::to_integer<std::uint64_t>(bytes_[static_cast<std::size_t>(byte)])
                     << (8 * byte);
        }
        bytes_ = bytes_.subspan(static_cast<std::size_t>(bytes));
        return value;
    }

    [[nodiscard]] Result<void> finish() const {
        if (!bytes_.empty()) {
            return make_error(ErrorCode::Parse,
                              std::format("{} unread bytes after change payload", bytes_.size()));
        }
        return {};
    }

private:
    std::span<const std::byte> bytes_;
};

template <typename Payload>
[[nodiscard]] Result<ChangePayload> finish(const Reader& reader, Payload payload) {
    if (auto done = reader.finish(); !done) {
        return std::unexpected(std::move(done.error()));
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
    Writer writer;
    std::visit(
        [&](const auto& change) {
            using Payload = std::decay_t<decltype(change)>;
            if constexpr (std::is_same_v<Payload, SetElevation>) {
                writer.write(static_cast<std::uint64_t>(change.elevation), 8);
            } else if constexpr (std::is_same_v<Payload, Excavate>) {
                writer.write(static_cast<std::uint64_t>(change.depth), 8);
            } else {
                static_assert(std::is_same_v<Payload, SetBiome>);
                writer.write(std::to_underlying(change.biome), 1);
            }
        },
        payload);
    return std::move(writer).take();
}

Result<ChangePayload> decode_payload(std::uint8_t kind, std::span<const std::byte> bytes) {
    Reader reader{bytes};
    switch (static_cast<ChangeKind>(kind)) {
    case ChangeKind::SetElevation: {
        const auto elevation = reader.read(8);
        if (!elevation) {
            return std::unexpected(elevation.error());
        }
        return finish(reader, SetElevation{static_cast<std::int64_t>(*elevation)});
    }
    case ChangeKind::Excavate: {
        const auto depth = reader.read(8);
        if (!depth) {
            return std::unexpected(depth.error());
        }
        return finish(reader, Excavate{static_cast<std::int64_t>(*depth)});
    }
    case ChangeKind::SetBiome: {
        const auto biome = reader.read(1);
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
