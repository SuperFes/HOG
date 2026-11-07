#include <hog/core/bytes.hpp>
#include <hog/globe/volume_archive.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <format>
#include <iterator>
#include <optional>
#include <utility>

namespace hog::globe {

Result<void> MemoryVolumeArchive::write(std::span<const std::byte> header,
                                        std::span<const SavedColumn> columns) {
    header_.assign(header.begin(), header.end());
    for (const SavedColumn& column : columns) {
        columns_.insert_or_assign({column.face, column.i, column.j, column.level}, column.bytes);
    }
    return {};
}

Result<std::vector<std::byte>> MemoryVolumeArchive::header() const {
    return header_;
}

Result<std::optional<SavedColumn>> MemoryVolumeArchive::column(std::uint8_t face, std::uint32_t i,
                                                               std::uint32_t j,
                                                               std::uint8_t level) const {
    const auto found = columns_.find({face, i, j, level});
    if (found == columns_.end()) {
        return std::nullopt;
    }
    return SavedColumn{.face = face, .i = i, .j = j, .level = level, .bytes = found->second};
}

Result<std::vector<SavedColumn>> MemoryVolumeArchive::columns() const {
    std::vector<SavedColumn> all;
    all.reserve(columns_.size());
    for (const auto& [place, bytes] : columns_) {
        const auto& [face, i, j, level] = place;
        if (level == 0) {
            all.push_back({.face = face, .i = i, .j = j, .level = 0, .bytes = bytes});
        }
    }
    return all;
}

std::uint8_t level_at(double metres) noexcept {
    if (metres < finest_reach) {
        return 0;
    }
    const double doublings = std::floor(std::log2(metres / finest_reach));
    return static_cast<std::uint8_t>(std::min(1.0 + doublings, double{coarsest_level}));
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

Result<std::optional<VolumeOrigin>> archived_origin(const VolumeArchive& archive) {
    const auto header = archive.header();
    if (!header) {
        return std::unexpected(header.error());
    }
    if (header->empty()) {
        return std::nullopt;
    }
    return origin_from(*header);
}

Result<bool> made_from(const VolumeArchive& archive, const VolumeOrigin& origin) {
    const auto archived = archived_origin(archive);
    if (!archived) {
        return std::unexpected(archived.error());
    }
    if (!*archived) {
        return false;
    }
    const VolumeOrigin& saved = **archived;
    if (saved != origin) {
        return make_error(ErrorCode::InvalidArgument,
                          std::format("the world was made from seed {}, not {}, or other layers",
                                      saved.seed, origin.seed));
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
    return save_columns(volume, archive, origin, volume.unsaved());
}

std::vector<SavedColumn> saved_levels(const TileColumn& column, std::uint8_t face, std::uint32_t i,
                                      std::uint32_t j) {
    std::vector<SavedColumn> levels;
    levels.reserve(coarsest_level + 1);
    levels.push_back({.face = face, .i = i, .j = j, .level = 0, .bytes = column.encode()});
    TileColumn coarser = column.halved();
    for (int level = 1; level <= coarsest_level; ++level) {
        levels.push_back({.face = face,
                          .i = i,
                          .j = j,
                          .level = static_cast<std::uint8_t>(level),
                          .bytes = coarser.encode()});
        if (level < coarsest_level) {
            coarser = coarser.halved();
        }
    }
    return levels;
}

Result<std::vector<Site>> load_columns(Volume& volume, const VolumeArchive& archive,
                                       std::span<const Site> corners) {
    constexpr auto side = static_cast<std::uint32_t>(Volume::tile_side);
    std::vector<Site> loaded;
    for (const Site corner : corners) {
        if (volume.floor(corner)) {
            continue;
        }
        const auto column = archive.column(corner.face, corner.i / side, corner.j / side, 0);
        if (!column) {
            return std::unexpected(column.error());
        }
        if (!*column) {
            continue;
        }
        if (auto decoded = volume.decode_column(corner, (*column)->bytes); !decoded) {
            return make_error(decoded.error().code,
                              std::format("tile column {} {} {}: {}", corner.face, corner.i / side,
                                          corner.j / side, decoded.error().message));
        }
        loaded.push_back(corner);
    }
    return loaded;
}

Result<std::size_t> save_columns(Volume& volume, VolumeArchive& archive, const VolumeOrigin& origin,
                                 std::span<const Site> corners) {
    const std::vector<Site> all = volume.unsaved();
    std::vector<Site> unsaved;
    for (const Site corner : corners) {
        if (std::ranges::binary_search(all, corner)) {
            unsaved.push_back(corner);
        }
    }
    if (unsaved.empty()) {
        return 0;
    }
    constexpr auto side = static_cast<std::uint32_t>(Volume::tile_side);
    std::vector<SavedColumn> columns;
    columns.reserve(unsaved.size() * (coarsest_level + 1));
    for (const Site corner : unsaved) {
        const std::optional<TileColumn> column = volume.column(corner);
        if (!column) {
            continue;
        }
        std::ranges::move(saved_levels(*column, corner.face, corner.i / side, corner.j / side),
                          std::back_inserter(columns));
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
