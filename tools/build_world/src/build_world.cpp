#include <hog/build_world/build_world.hpp>
#include <hog/globe/volume.hpp>
#include <hog/globe/volume_archive.hpp>
#include <hog/globe/volume_build.hpp>

#include <algorithm>
#include <format>
#include <iterator>
#include <optional>
#include <span>
#include <utility>

namespace hog::build_world {

std::vector<Area> whole(const globe::Generator& generator) {
    std::vector<Area> areas;
    areas.reserve(6);
    for (std::uint8_t face = 0; face < 6; ++face) {
        areas.push_back({.face = face, .i = 0, .j = 0, .across = globe::tiles_across(generator)});
    }
    return areas;
}

namespace {

// The tile columns of `areas` the archive doesn't hold, counting those it does in `built`.
Result<std::vector<globe::SavedColumn>> wanted_in(const globe::VolumeArchive& archive,
                                                  const std::vector<Area>& areas, Built& built) {
    std::vector<globe::SavedColumn> wanted;
    for (const Area& area : areas) {
        for (std::uint32_t j = area.j; j < area.j + area.across; ++j) {
            for (std::uint32_t i = area.i; i < area.i + area.across; ++i) {
                const auto held = archive.column(area.face, i, j, 0);
                if (!held) {
                    return std::unexpected(held.error());
                }
                if (*held) {
                    ++built.skipped;
                } else {
                    wanted.push_back({.face = area.face, .i = i, .j = j, .bytes = {}});
                }
            }
        }
    }
    return wanted;
}

// Each tile column at every level, one after another.
std::vector<globe::SavedColumn> make(const globe::Generator& generator, jobs::JobSystem& jobs,
                                     std::span<const globe::SavedColumn> columns) {
    std::vector<std::vector<globe::SavedColumn>> made(columns.size());
    jobs.parallel_for(
        static_cast<std::uint32_t>(columns.size()), 1, [&](jobs::Range range, std::uint32_t) {
            for (std::uint32_t k = range.begin; k < range.end; ++k) {
                const globe::SavedColumn& column = columns[k];
                const std::optional<globe::TileColumn> tiles =
                    globe::generate_tiles(generator, column.face, column.i, column.j)
                        .column({.face = column.face,
                                 .i = column.i * globe::Volume::tile_side,
                                 .j = column.j * globe::Volume::tile_side});
                if (tiles) {
                    made[k] = globe::saved_levels(*tiles, column.face, column.i, column.j);
                }
            }
        });
    std::vector<globe::SavedColumn> all;
    all.reserve(columns.size() * (globe::coarsest_level + 1));
    for (std::vector<globe::SavedColumn>& levels : made) {
        std::ranges::move(levels, std::back_inserter(all));
    }
    return all;
}

} // namespace

Result<Built> build(const globe::Generator& generator, globe::VolumeArchive& archive,
                    jobs::JobSystem& jobs, const std::vector<Area>& areas, std::size_t batch,
                    const Progress& progress) {
    const globe::VolumeOrigin origin = globe::origin_of(generator);
    const auto written = globe::made_from(archive, origin);
    if (!written) {
        return std::unexpected(written.error());
    }
    const std::vector<std::byte> header = globe::volume_header(origin);
    Built built;
    auto wanted = wanted_in(archive, areas, built);
    if (!wanted) {
        return std::unexpected(wanted.error());
    }
    if (!*written) {
        if (auto saved = archive.write(header, {}); !saved) {
            return std::unexpected(saved.error());
        }
    }
    batch = std::max<std::size_t>(batch, 1);
    for (std::size_t first = 0; first < wanted->size(); first += batch) {
        const std::span<const globe::SavedColumn> some{wanted->begin() +
                                                           static_cast<std::ptrdiff_t>(first),
                                                       std::min(batch, wanted->size() - first)};
        const std::vector<globe::SavedColumn> levels = make(generator, jobs, some);
        if (auto saved = archive.write(header, levels); !saved) {
            return std::unexpected(saved.error());
        }
        for (const globe::SavedColumn& column : levels) {
            built.bytes += column.bytes.size();
        }
        built.made += some.size();
        if (progress) {
            progress(built.made, wanted->size());
        }
    }
    return built;
}

} // namespace hog::build_world
