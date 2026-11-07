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

Result<std::vector<SavedColumn>> MemoryVolumeArchive::columns() const {
    std::vector<SavedColumn> all;
    all.reserve(columns_.size());
    for (const auto& [place, bytes] : columns_) {
        const auto& [face, i, j] = place;
        all.push_back({.face = face, .i = i, .j = j, .bytes = bytes});
    }
    return all;
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

Result<bool> made_from(const VolumeArchive& archive, const VolumeOrigin& origin) {
    const auto header = archive.header();
    if (!header) {
        return std::unexpected(header.error());
    }
    if (header->empty()) {
        return false;
    }
    const auto saved = origin_from(*header);
    if (!saved) {
        return std::unexpected(saved.error());
    }
    if (*saved != origin) {
        return make_error(ErrorCode::InvalidArgument,
                          std::format("the world was made from seed {}, not {}, or other layers",
                                      saved->seed, origin.seed));
    }
    return true;
}

Result<Volume> load_volume(const VolumeArchive& archive, const VolumeOrigin& origin) {
    const auto made = made_from(archive, origin);
    if (!made) {
        return std::unexpected(made.error());
    }
    Volume volume;
    if (!*made) {
        return volume;
    }
    const auto columns = archive.columns();
    if (!columns) {
        return std::unexpected(columns.error());
    }
    constexpr auto side = static_cast<std::uint32_t>(Volume::tile_side);
    for (const SavedColumn& column : *columns) {
        const Site corner{.face = column.face, .i = column.i * side, .j = column.j * side};
        if (auto decoded = volume.decode_column(corner, column.bytes); !decoded) {
            return make_error(decoded.error().code,
                              std::format("tile column {} {} {}: {}", column.face, column.i,
                                          column.j, decoded.error().message));
        }
    }
    return volume;
}

Result<std::size_t> save_volume(Volume& volume, VolumeArchive& archive,
                                const VolumeOrigin& origin) {
    const std::vector<Site> unsaved = volume.unsaved();
    if (unsaved.empty()) {
        return 0;
    }
    constexpr auto side = static_cast<std::uint32_t>(Volume::tile_side);
    std::vector<SavedColumn> columns;
    columns.reserve(unsaved.size());
    for (const Site corner : unsaved) {
        columns.push_back({.face = corner.face,
                           .i = corner.i / side,
                           .j = corner.j / side,
                           .bytes = volume.encode_column(corner)});
    }
    if (auto written = archive.write(volume_header(origin), columns); !written) {
        return std::unexpected(written.error());
    }
    for (const Site corner : unsaved) {
        volume.saved(corner);
    }
    return unsaved.size();
}

} // namespace hog::globe
