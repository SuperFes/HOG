#include <hog/planet/projection.hpp>
#include <hog/terrain/shore.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>

namespace hog::terrain {

ShoreFrame shore_frame(DVec3 eye, double radius) {
    const int level = planet::level_for(shore_anchoring, radius);
    const DVec3 anchor = planet::cell_center(planet::cell_at(eye, level));
    // Any way across will do at the poles, where east has none.
    DVec3 across = glm::cross(DVec3{0.0, 0.0, 1.0}, anchor);
    if (glm::length(across) < 1e-6) {
        across = glm::cross(DVec3{1.0, 0.0, 0.0}, anchor);
    }
    const DVec3 east = glm::normalize(across);
    return {.anchor = anchor, .east = east, .north = glm::cross(anchor, east)};
}

DVec3 shore_direction(const ShoreFrame& frame, double radius, DVec2 along) {
    return glm::normalize((frame.anchor * radius) + (frame.east * along.x) +
                          (frame.north * along.y));
}

DVec2 shore_along(const ShoreFrame& frame, double radius, DVec3 direction) {
    const double scale = radius / glm::dot(direction, frame.anchor);
    return DVec2{glm::dot(direction, frame.east), glm::dot(direction, frame.north)} * scale;
}

ShoreGround shore_ground(const planet::Elevation& terrain, const ShoreFrame& frame, double radius,
                         double spacing) {
    ShoreGround ground;
    ground.elevations.reserve(static_cast<std::size_t>(shore_size) * shore_size);
    ground.lakes.reserve(ground.elevations.capacity());
    const double first = (0.5 - (shore_size / 2.0)) * spacing;
    for (int row = 0; row < shore_size; ++row) {
        for (int column = 0; column < shore_size; ++column) {
            const planet::CellId cell = planet::cell_at(shore_direction(
                frame, radius, {first + (column * spacing), first + (row * spacing)}));
            const std::int64_t elevation = terrain.elevation(cell);
            const auto lake = elevation > 0 ? terrain.rivers().lake(cell, elevation) : std::nullopt;
            ground.elevations.push_back(static_cast<float>(planet::fixed_to_double(elevation)));
            ground.lakes.push_back(lake ? static_cast<float>(planet::fixed_to_double(*lake))
                                        : 0.0F);
        }
    }
    return ground;
}

std::vector<float> shore_depths(const ShoreGround& ground) {
    std::vector<float> depths;
    depths.reserve(ground.elevations.size());
    for (std::size_t at = 0; at < ground.elevations.size(); ++at) {
        depths.push_back(ground.lakes[at] > 0.0F ? shore_highest
                                                 : std::max(-ground.elevations[at], shore_highest));
    }
    return depths;
}

namespace {

constexpr auto shore_texels = static_cast<std::size_t>(shore_size) * shore_size;

// Water at least this deep is taken as the open sea.
constexpr float open_sea = 20.0F; // metres

// Spreads what the sea reaches from the texels marked to every texel lower than its reach.
void spread(std::span<const float> layer, std::vector<std::uint8_t>& reached) {
    std::vector<std::size_t> reaching;
    for (std::size_t at = 0; at < reached.size(); ++at) {
        if (reached[at] != 0) {
            reaching.push_back(at);
        }
    }
    const auto size = static_cast<std::size_t>(shore_size);
    while (!reaching.empty()) {
        const std::size_t at = reaching.back();
        reaching.pop_back();
        const std::size_t column = at % size;
        const std::size_t row = at / size;
        const auto reach = [&](bool inside, std::size_t next) {
            if (inside && reached[next] == 0 && layer[next] > -shore_reach) {
                reached[next] = 1;
                reaching.push_back(next);
            }
        };
        reach(column > 0, at - 1);
        reach(column + 1 < size, at + 1);
        reach(row > 0, at - size);
        reach(row + 1 < size, at + size);
    }
}

} // namespace

namespace {

// Whether the texel at `column` and `row` of layer `layer` is open sea: on the coarsest, deep or
// water at its edge; on any finer, water where the coarser layer's sea, `coarser`, reaches.
bool open_sea_at(std::span<const float> depths, std::size_t layer, std::size_t column,
                 std::size_t row, const std::vector<std::uint8_t>& coarser) {
    const auto size = static_cast<std::size_t>(shore_size);
    const std::size_t layers = depths.size() / shore_texels;
    const float depth = depths[(layer * shore_texels) + (row * size) + column];
    if (layer + 1 == layers) {
        const bool edge = row == 0 || column == 0 || row + 1 == size || column + 1 == size;
        return depth >= open_sea || (edge && depth > 0.0F);
    }
    if (depth <= 0.0F) {
        return false;
    }
    // The coarser layer's texel this one's centre lies in.
    const double ratio = shore_spacing.at(layer) / shore_spacing.at(layer + 1);
    const auto coarse = [&](std::size_t at) {
        return std::floor(((static_cast<double>(at) + 0.5 - (shore_size / 2.0)) * ratio) +
                          (shore_size / 2.0));
    };
    const double coarse_column = coarse(column);
    const double coarse_row = coarse(row);
    if (coarse_column < 0.0 || coarse_row < 0.0 || coarse_column >= shore_size ||
        coarse_row >= shore_size) {
        return false;
    }
    const std::size_t under =
        (static_cast<std::size_t>(coarse_row) * size) + static_cast<std::size_t>(coarse_column);
    return coarser[under] != 0 && depths[((layer + 1) * shore_texels) + under] > 0.0F;
}

} // namespace

std::vector<float> keep_to_sea_reach(std::span<float> depths) {
    const std::size_t layers = depths.size() / shore_texels;
    const auto size = static_cast<std::size_t>(shore_size);
    std::vector<float> still(depths.size(), shore_highest);
    std::vector<std::uint8_t> coarser;
    for (std::size_t layer = layers; layer-- > 0;) {
        std::vector<std::uint8_t> reached(shore_texels, 0);
        for (std::size_t row = 0; row < size; ++row) {
            for (std::size_t column = 0; column < size; ++column) {
                reached[(row * size) + column] =
                    open_sea_at(depths, layer, column, row, coarser) ? 1 : 0;
            }
        }
        const std::span<float> here = depths.subspan(layer * shore_texels, shore_texels);
        spread(here, reached);
        for (std::size_t at = 0; at < shore_texels; ++at) {
            if (reached[at] == 0) {
                if (here[at] > 0.0F) {
                    still[(layer * shore_texels) + at] = here[at];
                }
                here[at] = shore_highest;
            }
        }
        coarser = std::move(reached);
    }
    return still;
}

namespace {

// Of a layer's levels, `level`, over its ground, `ground`, and its lakes', `lake`: keeps over each
// texel off the lakes the highest level about it that its ground stands over.
void keep_lakes_out(std::span<float> level, std::span<const float> ground,
                    std::span<const float> lake) {
    const auto size = static_cast<std::size_t>(shore_size);
    const std::vector<float> before(level.begin(), level.end());
    for (std::size_t row = 0; row < size; ++row) {
        for (std::size_t column = 0; column < size; ++column) {
            const std::size_t at = (row * size) + column;
            if (lake[at] > 0.0F) {
                continue;
            }
            for (std::size_t down = std::max(row, std::size_t{1}) - 1;
                 down <= std::min(row + 1, size - 1); ++down) {
                for (std::size_t across = std::max(column, std::size_t{1}) - 1;
                     across <= std::min(column + 1, size - 1); ++across) {
                    const float around = before[(down * size) + across];
                    if (around <= ground[at]) {
                        level[at] = std::max(level[at], around);
                    }
                }
            }
        }
    }
}

} // namespace

std::vector<float> keep_lakes(std::span<const float> elevations, std::span<const float> lakes,
                              std::span<float> still) {
    std::vector<float> levels(lakes.begin(), lakes.end());
    const std::size_t layers = levels.size() / shore_texels;
    for (std::size_t layer = 0; layer < layers; ++layer) {
        const std::span<float> level =
            std::span{levels}.subspan(layer * shore_texels, shore_texels);
        const std::span<const float> ground =
            elevations.subspan(layer * shore_texels, shore_texels);
        // A step out from the shore at a time.
        for (int out = 0; out < lake_margin; ++out) {
            keep_lakes_out(level, ground, lakes.subspan(layer * shore_texels, shore_texels));
        }
        for (std::size_t at = 0; at < shore_texels; ++at) {
            if (level[at] > 0.0F) {
                still[(layer * shore_texels) + at] =
                    std::max(level[at] - ground[at], shore_highest);
            }
        }
    }
    return levels;
}

namespace {

// The step to the texel upwind of a wind blowing towards each heading.
constexpr std::array<std::array<int, 2>, fetch_headings> upwind{
    {{-1, 0}, {-1, -1}, {0, -1}, {1, -1}, {1, 0}, {1, 1}, {0, 1}, {-1, 1}}};

// Of a layer's fetch towards a heading, where its upwind step leaves the layer from `column` and
// `row`: the next coarser layer's at the point it leaves to, `coarser`, or the furthest kept from
// the coarsest.
float fetch_beyond(std::size_t layer, int column, int row, std::span<const float> coarser) {
    if (coarser.empty()) {
        return longest_fetch;
    }
    const double ratio = shore_spacing.at(layer) / shore_spacing.at(layer + 1);
    const auto coarse = [&](int at) {
        return static_cast<int>(
            std::floor(((at + 0.5 - (shore_size / 2.0)) * ratio) + (shore_size / 2.0)));
    };
    const int coarse_column = std::clamp(coarse(column), 0, shore_size - 1);
    const int coarse_row = std::clamp(coarse(row), 0, shore_size - 1);
    return coarser[(static_cast<std::size_t>(coarse_row) * shore_size) +
                   static_cast<std::size_t>(coarse_column)];
}

} // namespace

namespace {

// Of a layer's water, `wet`, its fetch towards `heading` into `here`, running on from `coarser`.
void sweep(std::size_t layer, int heading, const std::vector<std::uint8_t>& wet,
           std::span<float> here, std::span<const float> coarser) {
    const auto size = static_cast<std::size_t>(shore_size);
    const auto [dx, dy] = upwind.at(static_cast<std::size_t>(heading));
    const auto step = static_cast<float>(shore_spacing.at(layer) * std::hypot(dx, dy));
    const auto inside = [](int at) { return at >= 0 && at < shore_size; };
    // Upwind texels first.
    for (int across = 0; across < shore_size; ++across) {
        const int row = dy > 0 ? shore_size - 1 - across : across;
        for (int down = 0; down < shore_size; ++down) {
            const int column = dx > 0 ? shore_size - 1 - down : down;
            const std::size_t at =
                (static_cast<std::size_t>(row) * size) + static_cast<std::size_t>(column);
            if (wet[at] == 0) {
                continue;
            }
            const int from_column = column + dx;
            const int from_row = row + dy;
            const float before = inside(from_column) && inside(from_row)
                                     ? here[(static_cast<std::size_t>(from_row) * size) +
                                            static_cast<std::size_t>(from_column)]
                                     : fetch_beyond(layer, from_column, from_row, coarser);
            // From the shore halfway to the texel upwind, or on from that texel.
            here[at] = std::min(before > 0.0F ? before + step : step / 2.0F, longest_fetch);
        }
    }
}

} // namespace

std::vector<float> shore_fetches(std::span<const float> depths, std::span<const float> still) {
    const std::size_t layers = depths.size() / shore_texels;
    std::vector<float> fetches(layers * fetch_headings * shore_texels, 0.0F);
    const std::span<float> all{fetches};
    const auto plane = [&](std::size_t layer, int heading) {
        return all.subspan(((layer * fetch_headings) + static_cast<std::size_t>(heading)) *
                               shore_texels,
                           shore_texels);
    };
    for (std::size_t layer = layers; layer-- > 0;) {
        std::vector<std::uint8_t> wet(shore_texels);
        for (std::size_t at = 0; at < shore_texels; ++at) {
            const std::size_t in_layer = (layer * shore_texels) + at;
            wet[at] = depths[in_layer] > 0.0F || still[in_layer] > 0.0F ? 1 : 0;
        }
        for (int heading = 0; heading < fetch_headings; ++heading) {
            sweep(layer, heading, wet, plane(layer, heading),
                  layer + 1 < layers ? plane(layer + 1, heading) : std::span<float>{});
        }
    }
    return fetches;
}

void ShoreStream::follow(DVec3 eye) {
    wanted_ = shore_frame(eye, radius_);
    if (!working_ && wanted_ != started_) {
        start(*wanted_);
    }
}

std::vector<float> lake_levels(const ShoreDepths& shore) {
    std::vector<float> levels;
    const std::size_t texels = std::min(shore.levels.size(), shore_texels);
    for (std::size_t at = 0; at < texels; ++at) {
        if (shore.levels[at] > 0.0F && shore.still[at] > 0.0F) {
            levels.push_back(shore.levels[at]);
        }
    }
    std::ranges::sort(levels);
    const auto [last, end] = std::ranges::unique(levels);
    levels.erase(last, end);
    return levels;
}

double water_below(std::span<const float> levels, double height) {
    const auto above = std::ranges::upper_bound(
        levels, height, std::less{}, [](float level) { return static_cast<double>(level); });
    return above == levels.begin() ? 0.0 : static_cast<double>(*std::prev(above));
}

ShoreDepths work_out_shore(const planet::Elevation& terrain, const ShoreFrame& frame,
                           double radius) {
    ShoreDepths found{.frame = frame, .depths = {}, .still = {}, .levels = {}, .fetches = {}};
    std::vector<float> elevations;
    std::vector<float> lakes;
    for (const double spacing : shore_spacing) {
        const ShoreGround ground = shore_ground(terrain, frame, radius, spacing);
        const std::vector<float> layer = shore_depths(ground);
        found.depths.insert(found.depths.end(), layer.begin(), layer.end());
        elevations.insert(elevations.end(), ground.elevations.begin(), ground.elevations.end());
        lakes.insert(lakes.end(), ground.lakes.begin(), ground.lakes.end());
    }
    found.still = keep_to_sea_reach(found.depths);
    found.levels = keep_lakes(elevations, lakes, found.still);
    found.fetches = shore_fetches(found.depths, found.still);
    return found;
}

void ShoreStream::start(const ShoreFrame& frame) {
    started_ = frame;
    working_ = std::make_unique<jobs::Async<ShoreDepths>>(
        *jobs_, [terrain = terrain_, frame, radius = radius_] {
            return work_out_shore(*terrain, frame, radius);
        });
}

std::optional<ShoreDepths> ShoreStream::collect() {
    if (!working_ || !working_->done()) {
        return std::nullopt;
    }
    ShoreDepths found = working_->take();
    working_.reset();
    if (wanted_ && wanted_ != started_) {
        start(*wanted_);
    }
    return found;
}

std::optional<ShoreDepths> ShoreStream::finish() {
    std::optional<ShoreDepths> last;
    while (working_) {
        last = working_->take();
        working_.reset();
        if (wanted_ && wanted_ != started_) {
            start(*wanted_);
        }
    }
    return last;
}

} // namespace hog::terrain
