#include <hog/render/cloud_noise.hpp>
#include <hog/render/streamed_image.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <span>
#include <utility>

namespace hog::render {

namespace {

constexpr std::uint32_t billows_seed = 1;
constexpr std::array<std::uint32_t, 3> wearing_seeds{2, 3, 4};
constexpr std::array<std::uint32_t, 3> detail_seeds{5, 6, 7};
// Lattice cells across the billows' coarsest gradient noise, and how many finer octaves follow.
constexpr int billows_period = 4;
constexpr int billows_octaves = 4;

std::uint32_t hashed(std::int64_t x, std::int64_t y, std::int64_t z, std::uint32_t seed) noexcept {
    std::uint32_t h = (static_cast<std::uint32_t>(x) * 73856093U) ^
                      (static_cast<std::uint32_t>(y) * 19349663U) ^
                      (static_cast<std::uint32_t>(z) * 83492791U) ^ (seed * 2654435761U);
    h ^= h >> 16U;
    h *= 0x7feb352dU;
    h ^= h >> 15U;
    h *= 0x846ca68bU;
    h ^= h >> 16U;
    return h;
}

std::int64_t wrapped(std::int64_t at, int period) noexcept {
    return ((at % period) + period) % period;
}

double unit(std::uint32_t bits) noexcept {
    return static_cast<double>(bits & 0xffffffU) / static_cast<double>(0x1000000U);
}

double fade(double t) noexcept {
    return t * t * t * ((t * ((t * 6.0) - 15.0)) + 10.0);
}

// The points tiled_cells() scatters, one to each cell.
class Scattered {
public:
    Scattered(int cells, std::uint32_t seed) : cells_{cells} {
        points_.reserve(static_cast<std::size_t>(cells) * static_cast<std::size_t>(cells) *
                        static_cast<std::size_t>(cells));
        for (int z = 0; z < cells; ++z) {
            for (int y = 0; y < cells; ++y) {
                for (int x = 0; x < cells; ++x) {
                    points_.emplace_back(unit(hashed(x, y, z, seed)),
                                         unit(hashed(x, y, z, seed + 101)),
                                         unit(hashed(x, y, z, seed + 202)));
                }
            }
        }
    }

    // As tiled_cells().
    [[nodiscard]] double nearness(DVec3 at) const noexcept {
        const DVec3 scaled = at * static_cast<double>(cells_);
        const DVec3 cell = glm::floor(scaled);
        double nearest = 1.0;
        for (int dz = -1; dz <= 1; ++dz) {
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    const DVec3 next = cell + DVec3{dx, dy, dz};
                    nearest = std::min(nearest, glm::length(next + point(next) - scaled));
                }
            }
        }
        return 1.0 - nearest;
    }

private:
    [[nodiscard]] DVec3 point(DVec3 cell) const noexcept {
        const auto x = static_cast<std::size_t>(wrapped(static_cast<std::int64_t>(cell.x), cells_));
        const auto y = static_cast<std::size_t>(wrapped(static_cast<std::int64_t>(cell.y), cells_));
        const auto z = static_cast<std::size_t>(wrapped(static_cast<std::int64_t>(cell.z), cells_));
        const auto side = static_cast<std::size_t>(cells_);
        return points_[(((z * side) + y) * side) + x];
    }

    int cells_;
    std::vector<DVec3> points_;
};

// Cells at some size, twice and four times as fine, the coarsest weighing most.
class Worn {
public:
    Worn(int cells, std::uint32_t seed)
        : coarse_{cells, seed}, middle_{cells * 2, seed + 16}, fine_{cells * 4, seed + 32} {}

    [[nodiscard]] double at(DVec3 at) const noexcept {
        return (0.625 * coarse_.nearness(at)) + (0.25 * middle_.nearness(at)) +
               (0.125 * fine_.nearness(at));
    }

private:
    Scattered coarse_;
    Scattered middle_;
    Scattered fine_;
};

// From 0 to 1.
double billows(DVec3 at) noexcept {
    double sum = 0.0;
    double amplitude = 1.0;
    double total = 0.0;
    for (int octave = 0; octave < billows_octaves; ++octave) {
        sum += amplitude * tiled_gradient(at, billows_period << octave,
                                          billows_seed + static_cast<std::uint32_t>(octave));
        total += amplitude;
        amplitude *= 0.5;
    }
    return std::clamp((sum / total * 0.5) + 0.5, 0.0, 1.0);
}

std::byte to_texel(double value) noexcept {
    return static_cast<std::byte>(std::lround(std::clamp(value, 0.0, 1.0) * 255.0));
}

// Of a volume `size` texels a side, the next mip down.
std::vector<std::byte> halved(std::span<const std::byte> above, std::uint32_t size) {
    const std::uint32_t below = size / 2;
    const auto at = [&](std::uint32_t x, std::uint32_t y, std::uint32_t z, std::uint32_t c) {
        return std::to_integer<int>(
            above[((((((std::size_t{z} * size) + y) * size) + x) * 4) + c)]);
    };
    std::vector<std::byte> mip;
    mip.reserve(std::size_t{below} * below * below * 4);
    for (std::uint32_t z = 0; z < below; ++z) {
        for (std::uint32_t y = 0; y < below; ++y) {
            for (std::uint32_t x = 0; x < below; ++x) {
                for (std::uint32_t c = 0; c < 4; ++c) {
                    int sum = 0;
                    for (std::uint32_t corner = 0; corner < 8; ++corner) {
                        sum += at((2 * x) + (corner & 1U), (2 * y) + ((corner >> 1U) & 1U),
                                  (2 * z) + (corner >> 2U), c);
                    }
                    mip.push_back(static_cast<std::byte>((sum + 4) / 8));
                }
            }
        }
    }
    return mip;
}

// The volume of `size` texels a side and its mips, each texel's four channels from `texel` at
// its middle, on `jobs` if given.
std::vector<std::byte> baked(std::uint32_t size, std::uint32_t levels,
                             const std::function<std::array<double, 4>(DVec3)>& texel,
                             jobs::JobSystem* jobs) {
    const std::size_t slice = std::size_t{size} * size * 4;
    std::vector<std::byte> out(slice * size);
    const auto bake = [&](jobs::Range slices, std::uint32_t) {
        for (std::uint32_t z = slices.begin; z < slices.end; ++z) {
            std::size_t at = z * slice;
            for (std::uint32_t y = 0; y < size; ++y) {
                for (std::uint32_t x = 0; x < size; ++x) {
                    const DVec3 middle = (DVec3{x, y, z} + 0.5) / static_cast<double>(size);
                    for (const double channel : texel(middle)) {
                        out[at++] = to_texel(channel);
                    }
                }
            }
        }
    };
    if (jobs != nullptr) {
        jobs->parallel_for(size, 1, bake);
    } else {
        bake({.begin = 0, .end = size}, 0);
    }
    std::size_t from = 0;
    for (std::uint32_t level = 1, above = size; level < levels && above > 1; ++level) {
        const std::vector<std::byte> mip = halved(std::span{out}.subspan(from), above);
        from = out.size();
        out.insert(out.end(), mip.begin(), mip.end());
        above /= 2;
    }
    return out;
}

} // namespace

double tiled_cells(DVec3 at, int cells, std::uint32_t seed) noexcept {
    return Scattered{cells, seed}.nearness(at);
}

double tiled_gradient(DVec3 at, int period, std::uint32_t seed) noexcept {
    const DVec3 scaled = at * static_cast<double>(period);
    const DVec3 cell = glm::floor(scaled);
    const DVec3 f = scaled - cell;
    // Towards the middles of a cube's edges.
    static constexpr std::array<DVec3, 12> gradients{
        DVec3{1, 1, 0}, DVec3{-1, 1, 0}, DVec3{1, -1, 0}, DVec3{-1, -1, 0},
        DVec3{1, 0, 1}, DVec3{-1, 0, 1}, DVec3{1, 0, -1}, DVec3{-1, 0, -1},
        DVec3{0, 1, 1}, DVec3{0, -1, 1}, DVec3{0, 1, -1}, DVec3{0, -1, -1}};
    const auto corner = [&](int dx, int dy, int dz) {
        const auto x = wrapped(static_cast<std::int64_t>(cell.x) + dx, period);
        const auto y = wrapped(static_cast<std::int64_t>(cell.y) + dy, period);
        const auto z = wrapped(static_cast<std::int64_t>(cell.z) + dz, period);
        return glm::dot(gradients.at(hashed(x, y, z, seed) % gradients.size()),
                        f - DVec3{dx, dy, dz});
    };
    const DVec3 u{fade(f.x), fade(f.y), fade(f.z)};
    const auto mix = [](double from, double to, double by) { return from + ((to - from) * by); };
    const double x00 = mix(corner(0, 0, 0), corner(1, 0, 0), u.x);
    const double x10 = mix(corner(0, 1, 0), corner(1, 1, 0), u.x);
    const double x01 = mix(corner(0, 0, 1), corner(1, 0, 1), u.x);
    const double x11 = mix(corner(0, 1, 1), corner(1, 1, 1), u.x);
    return mix(mix(x00, x10, u.y), mix(x01, x11, u.y), u.z);
}

std::vector<std::byte> shape_noise(std::uint32_t size, std::uint32_t levels,
                                   jobs::JobSystem* jobs) {
    const Worn wearing{4, wearing_seeds[0]};
    const Worn coarse{4, wearing_seeds[1]};
    const Worn middle{8, wearing_seeds[1]};
    const Worn fine{16, wearing_seeds[2]};
    return baked(
        size, levels,
        [&](DVec3 at) {
            // Billows worn where the cells are far: Hillaire's Perlin-Worley.
            const double cells = wearing.at(at);
            const double shape = (billows(at) - (cells - 1.0)) / (2.0 - cells);
            return std::array{shape, coarse.at(at), middle.at(at), fine.at(at)};
        },
        jobs);
}

std::vector<std::byte> detail_noise(std::uint32_t size, std::uint32_t levels,
                                    jobs::JobSystem* jobs) {
    const Worn coarse{2, detail_seeds[0]};
    const Worn middle{4, detail_seeds[1]};
    const Worn fine{8, detail_seeds[2]};
    return baked(
        size, levels,
        [&](DVec3 at) {
            const double first = coarse.at(at);
            const double second = middle.at(at);
            const double third = fine.at(at);
            return std::array{first, second, third,
                              (0.625 * first) + (0.25 * second) + (0.125 * third)};
        },
        jobs);
}

Result<CloudNoise> make_cloud_noise(Device& device, jobs::JobSystem& jobs) {
    const auto volume = [&](std::uint32_t size, const std::vector<std::byte>& texels) {
        return make_filled_image(device,
                                 {.format = VK_FORMAT_R8G8B8A8_UNORM,
                                  .extent = {.width = size, .height = size},
                                  .levels = noise_levels,
                                  .depth = size},
                                 texels);
    };
    auto shape = volume(shape_noise_size, shape_noise(shape_noise_size, noise_levels, &jobs));
    if (!shape) {
        return std::unexpected(std::move(shape).error());
    }
    auto detail = volume(detail_noise_size, detail_noise(detail_noise_size, noise_levels, &jobs));
    if (!detail) {
        return std::unexpected(std::move(detail).error());
    }
    return CloudNoise{.shape = std::move(shape).value(), .detail = std::move(detail).value()};
}

} // namespace hog::render
