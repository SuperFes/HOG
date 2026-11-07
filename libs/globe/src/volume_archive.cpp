#include <hog/core/bytes.hpp>
#include <hog/globe/volume_archive.hpp>

#include <bit>
#include <format>
#include <utility>

namespace hog::globe {

Result<void> MemoryVolumeArchive::write(std::span<const std::byte> header,
                                        std::span<const SavedColumn> columns) {
    header_.assign(header.begin(), header.end());
    for (const SavedColumn& column : columns) {
        columns_.insert_or_assign({column.face, column.i, column.j}, column.bytes);
    }
    return {};
}

Result<std::vector<std::byte>> MemoryVolumeArchive::header() const {
    return header_;
}

Result<std::optional<SavedColumn>> MemoryVolumeArchive::column(std::uint8_t face, std::uint32_t i,
                                                               std::uint32_t j) const {
    const auto found = columns_.find({face, i, j});
    if (found == columns_.end()) {
        return std::nullopt;
    }
    return SavedColumn{.face = face, .i = i, .j = j, .bytes = found->second};
}

VolumeOrigin origin_of(const Generator& generator) {
    return {.seed = generator.planet().seed, .layers = generator.layers()};
}

std::vector<std::byte> volume_header(const VolumeOrigin& origin) {
    ByteWriter out;
    out.write(volume_format);
    out.write(origin.seed);
    out.write(static_cast<std::uint8_t>(origin.layers.level));
    out.write(static_cast<std::uint32_t>(origin.layers.count));
    out.write(std::bit_cast<std::uint64_t>(origin.layers.floor));
    out.write(std::bit_cast<std::uint64_t>(origin.layers.thickness));
    return std::move(out).take();
}

Result<VolumeOrigin> origin_from(std::span<const std::byte> header) {
    ByteReader in{header};
    const auto format = in.read<std::uint16_t>();
    if (!format) {
        return std::unexpected(format.error());
    }
    if (*format != volume_format) {
        return make_error(ErrorCode::Unsupported, std::format("a volume saved in format {}, not {}",
                                                              *format, volume_format));
    }
    const auto seed = in.read<std::uint64_t>();
    const auto level = in.read<std::uint8_t>();
    const auto count = in.read<std::uint32_t>();
    const auto floor = in.read<std::uint64_t>();
    const auto thickness = in.read<std::uint64_t>();
    if (!seed || !level || !count || !floor || !thickness) {
        return make_error(ErrorCode::Parse, "a volume header cut short");
    }
    if (auto finished = in.finish(); !finished) {
        return std::unexpected(finished.error());
    }
    return VolumeOrigin{.seed = *seed,
                        .layers = {.level = *level,
                                   .floor = std::bit_cast<double>(*floor),
                                   .thickness = std::bit_cast<double>(*thickness),
                                   .count = static_cast<int>(*count)}};
}

} // namespace hog::globe
