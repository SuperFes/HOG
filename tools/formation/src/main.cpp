#include <hog/core/image.hpp>
#include <hog/formation/bedrock.hpp>
#include <hog/formation/body.hpp>
#include <hog/formation/climate.hpp>
#include <hog/formation/drifts.hpp>
#include <hog/formation/ice.hpp>
#include <hog/formation/lakes.hpp>
#include <hog/formation/landforms.hpp>
#include <hog/formation/ores.hpp>
#include <hog/formation/plates.hpp>
#include <hog/formation/rivers.hpp>
#include <hog/formation/sediment.hpp>
#include <hog/formation/soil.hpp>
#include <hog/formation/sphere.hpp>
#include <hog/formation_lab/pictures.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <format>
#include <functional>
#include <iostream>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using hog::DVec3;
using hog::formation::Sphere;
using hog::formation_lab::Rgb;

constexpr std::string_view usage =
    "usage: hog-formation [--seed N] [--plates N] [--continents N] [--land SHARE]\n"
    "                     [--speed DEG_PER_MYR] [--age MYR] [--radius M]\n"
    "                     [--layout SPLITS] [--detail SPLITS] [--width N] [--out DIR]\n"
    "Forms a planet in stages and writes a map of each stage's result to DIR:\n"
    "plates.png (plates, light crust, and how they meet), base.png (how the crust stands),\n"
    "uplift.png (where rock is pushed up), temperature.png and rain.png (yearly means),\n"
    "height.png (land worn by its rivers and carved by ice), ice.png (ice now, and what the\n"
    "coldest times' ice carved), sediment.png (what rivers laid down), soil.png (how deep\n"
    "soil lies) and topsoil.png (how dark its top is with humus, and where peat lies).\n";

struct Options {
    hog::formation::Body body;
    int layout = 5; // splits of the sphere the plates are laid out on
    int detail = 7; // splits of the sphere the results are held on
    int width = 2048;
    std::filesystem::path out = ".";
};

std::optional<double> number(std::string_view text) {
    const std::string copy{text};
    char* end = nullptr;
    const double value = std::strtod(copy.c_str(), &end);
    if (copy.empty() || end != std::to_address(copy.end()) || !std::isfinite(value)) {
        return std::nullopt;
    }
    return value;
}

bool apply(Options& options, std::string_view name, std::string_view value) {
    if (name == "--out") {
        options.out = value;
        return true;
    }
    const auto parsed = number(value);
    if (!parsed) {
        return false;
    }
    const double n = *parsed;
    const auto whole = [n] { return static_cast<int>(std::lround(n)); };
    if (name == "--seed") {
        options.body.seed = static_cast<std::uint64_t>(std::llround(std::abs(n)));
    } else if (name == "--plates") {
        options.body.plates = std::clamp(whole(), 1, 1000);
    } else if (name == "--continents") {
        options.body.continents = std::clamp(whole(), 1, 1000);
    } else if (name == "--land") {
        options.body.land_share = std::clamp(n, 0.0, 1.0);
    } else if (name == "--speed") {
        options.body.plate_speed = std::max(n, 0.0);
    } else if (name == "--age") {
        options.body.boundary_age = std::max(n, 0.0);
    } else if (name == "--radius") {
        options.body.radius = std::max(n, 1.0);
    } else if (name == "--layout") {
        options.layout = std::clamp(whole(), 2, Sphere::max_subdivisions);
    } else if (name == "--detail") {
        options.detail = std::clamp(whole(), 2, Sphere::max_subdivisions);
    } else if (name == "--width") {
        options.width = std::clamp(whole(), 64, 16'384);
    } else {
        return false;
    }
    return true;
}

bool write(const std::filesystem::path& path, const hog::Image& image) {
    if (const auto written = hog::write_png(path, image); !written) {
        std::cerr << "hog-formation: can't write " << path.string() << "\n";
        return false;
    }
    std::cout << "wrote " << path.string() << "\n";
    return true;
}

double seconds_since(std::chrono::steady_clock::time_point from) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - from).count();
}

// What every map is drawn with: where it goes, how wide, and the sphere results are held on.
struct Canvas {
    const Options* options = nullptr;
    const Sphere* sphere = nullptr;
    double step = 0.0; // radians between the samples relief is lit from

    [[nodiscard]] bool
    draw(std::string_view name,
         const std::function<Rgb(const DVec3&, const Sphere::Spot&)>& colour) const {
        std::uint32_t start = 0;
        return write(options->out / name,
                     hog::formation_lab::map_picture(options->width, [&](const DVec3& direction) {
                         const Sphere::Spot spot = sphere->locate(direction, start);
                         start = spot.corners[0];
                         return colour(direction, spot);
                     }));
    }

    [[nodiscard]] double light(std::span<const double> heights, const DVec3& direction,
                               std::uint32_t& start) const {
        return hog::formation_lab::shade(*sphere, heights, direction, options->body.radius, step,
                                         1.0, start);
    }
};

int river_width(double share) {
    if (share > 0.1) {
        return 3;
    }
    return share > 0.02 ? 2 : 1;
}

bool draw_crust(const Canvas& canvas, const hog::formation::Crust& crust) {
    const Sphere& sphere = *canvas.sphere;
    bool ok = canvas.draw("plates.png", [&](const DVec3& direction, const Sphere::Spot& spot) {
        const std::uint32_t near = sphere.nearest(direction, spot.corners[0]);
        if (crust.meeting[near] != hog::formation::Meeting::None &&
            crust.boundary[near] < 1.2 * sphere.spacing()) {
            return hog::formation_lab::meeting_colour(crust.meeting[near]);
        }
        const Rgb plate = hog::formation_lab::plate_colour(crust.plate[near]);
        return hog::formation_lab::scaled(plate, 0.55 + (0.5 * crust.light[near]));
    });
    std::uint32_t lit = 0;
    ok = canvas.draw("base.png", [&](const DVec3& direction, const Sphere::Spot& spot) {
        const double height = sphere.sample(crust.base, spot);
        const double light = canvas.light(crust.base, direction, lit);
        return hog::formation_lab::scaled(hog::formation_lab::height_colour(height),
                                          height < 0.0 ? 1.0 : light);
    }) && ok;
    const double most = std::max(1e-12, *std::ranges::max_element(crust.uplift));
    static constexpr std::array<Rgb, 5> heat{
        hog::formation_lab::rgb(20, 20, 30), hog::formation_lab::rgb(60, 40, 120),
        hog::formation_lab::rgb(180, 50, 90), hog::formation_lab::rgb(240, 140, 40),
        hog::formation_lab::rgb(250, 240, 160)};
    return canvas.draw("uplift.png", [&](const DVec3&, const Sphere::Spot& spot) {
        const double share = sphere.sample(crust.uplift, spot) / most;
        return hog::formation_lab::ramp(std::sqrt(std::max(share, 0.0)), heat);
    }) && ok;
}

bool draw_climate(const Canvas& canvas, const hog::formation::Crust& crust,
                  const hog::formation::Climate& climate) {
    const Sphere& sphere = *canvas.sphere;
    static constexpr std::array<Rgb, 6> cold_to_hot{
        hog::formation_lab::rgb(40, 40, 140),   hog::formation_lab::rgb(80, 140, 220),
        hog::formation_lab::rgb(220, 230, 230), hog::formation_lab::rgb(240, 200, 80),
        hog::formation_lab::rgb(220, 90, 40),   hog::formation_lab::rgb(120, 20, 20)};
    const bool ok = canvas.draw("temperature.png", [&](const DVec3&, const Sphere::Spot& spot) {
        const double celsius = sphere.sample(climate.temperature, spot);
        const Rgb colour = hog::formation_lab::ramp((celsius + 40.0) / 80.0, cold_to_hot);
        const bool sea = sphere.sample(crust.base, spot) < 0.0;
        return sea ? hog::formation_lab::scaled(colour, 0.7) : colour;
    });
    static constexpr std::array<Rgb, 5> dry_to_wet{
        hog::formation_lab::rgb(150, 90, 40), hog::formation_lab::rgb(220, 200, 120),
        hog::formation_lab::rgb(120, 190, 90), hog::formation_lab::rgb(30, 120, 70),
        hog::formation_lab::rgb(20, 60, 140)};
    return canvas.draw("rain.png", [&](const DVec3&, const Sphere::Spot& spot) {
        const double metres = sphere.sample(climate.rain, spot);
        const Rgb colour =
            hog::formation_lab::ramp(std::sqrt(std::max(metres, 0.0) / 3.0), dry_to_wet);
        const bool sea = sphere.sample(crust.base, spot) < 0.0;
        return sea ? hog::formation_lab::scaled(colour, 0.45) : colour;
    }) && ok;
}

// Standing water over the land, if any: open lakes blue, closed ones greener, salt flats white.
// A lake filled in by what settles in it is a plain now.
std::optional<Rgb> water_colour(const hog::formation::Lakes& lakes,
                                const hog::formation::Sediment& sediment, std::uint32_t near) {
    if (lakes.salt_flat[near] != 0) {
        return hog::formation_lab::rgb(235, 230, 215);
    }
    if (lakes.lake[near] == hog::formation::Lakes::none || sediment.lake_depth[near] <= 0.0) {
        return std::nullopt;
    }
    return lakes.lakes[lakes.lake[near]].open ? hog::formation_lab::rgb(50, 110, 200)
                                              : hog::formation_lab::rgb(60, 150, 150);
}

constexpr Rgb ice_white = hog::formation_lab::rgb(235, 240, 248);

Rgb mixed(const Rgb& from, const Rgb& to, double share) {
    return hog::formation_lab::ramp(share, std::array<Rgb, 2>{from, to});
}

// `rivers` as ice left the land, so the sea reaches into the troughs it carved.
bool draw_rivers(const Canvas& canvas, const hog::formation::Rivers& rivers,
                 const hog::formation::Ice& ice, const hog::formation::Lakes& lakes,
                 const hog::formation::Sediment& sediment) {
    const Sphere& sphere = *canvas.sphere;
    std::uint32_t start = 0;
    std::uint32_t lit = 0;
    hog::Image map =
        hog::formation_lab::map_picture(canvas.options->width, [&](const DVec3& direction) {
            const Sphere::Spot spot = sphere.locate(direction, start);
            start = spot.corners[0];
            const double height = sphere.sample(rivers.height, spot);
            const std::uint32_t near = sphere.nearest(direction, start);
            if (height < 0.0 && rivers.receiver[near] == near) {
                return mixed(hog::formation_lab::height_colour(height), ice_white,
                             0.8 * sphere.sample(ice.sea_ice, spot));
            }
            if (ice.thickness[near] > 0.0) {
                return hog::formation_lab::scaled(ice_white,
                                                  canvas.light(rivers.height, direction, lit));
            }
            if (const auto water = water_colour(lakes, sediment, near)) {
                return *water;
            }
            return hog::formation_lab::scaled(hog::formation_lab::height_colour(height),
                                              canvas.light(rivers.height, direction, lit));
        });
    // Each river drawn from a point to where its water goes, wider as more passes.
    double biggest = 1.0;
    for (std::uint32_t point = 0; point < sphere.size(); ++point) {
        if (rivers.receiver[point] != point) {
            biggest = std::max(biggest, lakes.discharge[point]);
        }
    }
    for (std::uint32_t point = 0; point < sphere.size(); ++point) {
        const double share = lakes.discharge[point] / biggest;
        const bool in_lake = lakes.lake[point] != hog::formation::Lakes::none;
        const bool land = rivers.receiver[point] != point && ice.thickness[point] <= 0.0;
        if (land && !in_lake && share >= 0.003) {
            hog::formation_lab::draw_line(map, sphere.point(point),
                                          sphere.point(rivers.receiver[point]),
                                          hog::formation_lab::rgb(40, 90, 200), river_width(share));
        }
    }
    return write(canvas.options->out / "height.png", map);
}

// Ice lying now white; where only the coldest times' ice lay, purple as deep as it carved; the sea
// let into carved troughs teal; the sea paler as it lies frozen more of the year.
bool draw_ice(const Canvas& canvas, const hog::formation::Crust& crust,
              const hog::formation::Rivers& rivers, const hog::formation::Ice& ice) {
    const Sphere& sphere = *canvas.sphere;
    static constexpr std::array<Rgb, 3> shallow_to_deep{hog::formation_lab::rgb(200, 185, 225),
                                                        hog::formation_lab::rgb(130, 90, 190),
                                                        hog::formation_lab::rgb(60, 25, 110)};
    std::uint32_t lit = 0;
    return canvas.draw("ice.png", [&](const DVec3& direction, const Sphere::Spot& spot) {
        const std::uint32_t near = sphere.nearest(direction, spot.corners[0]);
        if (rivers.receiver[near] == near) {
            if (crust.sea[near] == 0) {
                return hog::formation_lab::rgb(40, 150, 160);
            }
            return mixed(hog::formation_lab::rgb(20, 30, 60), ice_white,
                         0.8 * sphere.sample(ice.sea_ice, spot));
        }
        const double light = canvas.light(rivers.height, direction, lit);
        if (ice.thickness[near] > 0.0) {
            return hog::formation_lab::scaled(ice_white, light);
        }
        if (ice.coldest[near] > 0.0) {
            return hog::formation_lab::scaled(
                hog::formation_lab::ramp(ice.carved[near] / 1'500.0, shallow_to_deep), light);
        }
        return hog::formation_lab::scaled(hog::formation_lab::rgb(120, 115, 105), light);
    });
}

// The rock at the surface once worn: lava near black, flat layered rock tan (paler grey where much
// of it dissolves), folded layers purple, the light crust's basement pink.
bool draw_bedrock(const Canvas& canvas, const hog::formation::Rivers& rivers,
                  const hog::formation::Bedrock& bedrock, const hog::formation::Outcrop& outcrop) {
    using hog::formation::Rock;
    const Sphere& sphere = *canvas.sphere;
    std::uint32_t lit = 0;
    return canvas.draw("bedrock.png", [&](const DVec3& direction, const Sphere::Spot& spot) {
        const std::uint32_t near = sphere.nearest(direction, spot.corners[0]);
        if (rivers.receiver[near] == near) {
            return hog::formation_lab::rgb(20, 30, 60);
        }
        const double light = canvas.light(rivers.height, direction, lit);
        Rgb colour = hog::formation_lab::rgb(190, 110, 100);
        switch (outcrop.rock[near]) {
        case Rock::Lava:
            colour = hog::formation_lab::rgb(60, 55, 60);
            break;
        case Rock::Layered:
            colour = mixed(hog::formation_lab::rgb(205, 170, 115),
                           hog::formation_lab::rgb(185, 195, 200), bedrock.soluble[near]);
            break;
        case Rock::Folded:
            colour = hog::formation_lab::rgb(140, 100, 150);
            break;
        case Rock::Basement:
        case Rock::SeaFloor:
            break;
        }
        return hog::formation_lab::scaled(colour, light);
    });
}

// The landforms in the order drawn and reported, with their colours.
struct Form {
    std::string_view name;
    std::vector<double> hog::formation::Landforms::* field;
    Rgb colour;
};

constexpr std::array<Form, 10> forms{{
    {.name = "canyons",
     .field = &hog::formation::Landforms::canyons,
     .colour = hog::formation_lab::rgb(225, 95, 40)},
    {.name = "mesas",
     .field = &hog::formation::Landforms::mesas,
     .colour = hog::formation_lab::rgb(140, 55, 35)},
    {.name = "escarpments",
     .field = &hog::formation::Landforms::escarpments,
     .colour = hog::formation_lab::rgb(230, 200, 80)},
    {.name = "badlands",
     .field = &hog::formation::Landforms::badlands,
     .colour = hog::formation_lab::rgb(205, 160, 195)},
    {.name = "karst",
     .field = &hog::formation::Landforms::karst,
     .colour = hog::formation_lab::rgb(70, 175, 200)},
    {.name = "sea cliffs",
     .field = &hog::formation::Landforms::sea_cliffs,
     .colour = hog::formation_lab::rgb(245, 245, 245)},
    {.name = "glacial",
     .field = &hog::formation::Landforms::glacial,
     .colour = hog::formation_lab::rgb(110, 130, 235)},
    {.name = "fault scarps",
     .field = &hog::formation::Landforms::fault_scarps,
     .colour = hog::formation_lab::rgb(230, 40, 160)},
    {.name = "volcanoes",
     .field = &hog::formation::Landforms::volcanoes,
     .colour = hog::formation_lab::rgb(170, 15, 15)},
    {.name = "scree",
     .field = &hog::formation::Landforms::scree,
     .colour = hog::formation_lab::rgb(150, 150, 150)},
}};

// Each point in the colour of its likeliest landform, where one is likely at all.
bool draw_landforms(const Canvas& canvas, const hog::formation::Rivers& rivers,
                    const hog::formation::Landforms& landforms) {
    const Sphere& sphere = *canvas.sphere;
    std::uint32_t lit = 0;
    return canvas.draw("landforms.png", [&](const DVec3& direction, const Sphere::Spot& spot) {
        const std::uint32_t near = sphere.nearest(direction, spot.corners[0]);
        if (rivers.receiver[near] == near) {
            return hog::formation_lab::rgb(20, 30, 60);
        }
        Rgb colour = hog::formation_lab::rgb(110, 120, 100);
        double likeliest = 0.25;
        for (const Form& form : forms) {
            const double likely = (landforms.*form.field)[near];
            if (likely > likeliest) {
                likeliest = likely;
                colour = form.colour;
            }
        }
        return hog::formation_lab::scaled(colour, canvas.light(rivers.height, direction, lit));
    });
}

void report_landforms(const Sphere& sphere, const hog::formation::Rivers& rivers,
                      const hog::formation::Landforms& landforms) {
    double land = 0.0;
    std::array<double, forms.size()> area{};
    for (std::uint32_t point = 0; point < sphere.size(); ++point) {
        if (rivers.receiver[point] == point) {
            continue;
        }
        land += sphere.area(point);
        for (std::size_t form = 0; form < forms.size(); ++form) {
            area.at(form) +=
                (landforms.*forms.at(form).field)[point] > 0.3 ? sphere.area(point) : 0.0;
        }
    }
    std::string line = "landforms likely on the land:";
    for (std::size_t form = 0; form < forms.size(); ++form) {
        line += std::format(" {} {:.1f}%{}", forms.at(form).name,
                            100.0 * area.at(form) / std::max(land, 1e-12),
                            form + 1 < forms.size() ? "," : "");
    }
    std::cout << line << '\n';
}

// The ores in the order drawn and reported, with their colours.
struct Ore {
    std::string_view name;
    std::vector<double> hog::formation::Ores::* field;
    Rgb colour;
};

constexpr std::array<Ore, 11> ores{{
    {.name = "copper",
     .field = &hog::formation::Ores::copper,
     .colour = hog::formation_lab::rgb(40, 190, 150)},
    {.name = "gold",
     .field = &hog::formation::Ores::gold,
     .colour = hog::formation_lab::rgb(240, 200, 40)},
    {.name = "placers",
     .field = &hog::formation::Ores::placers,
     .colour = hog::formation_lab::rgb(250, 240, 150)},
    {.name = "banded iron",
     .field = &hog::formation::Ores::banded_iron,
     .colour = hog::formation_lab::rgb(170, 40, 30)},
    {.name = "laterite",
     .field = &hog::formation::Ores::laterite,
     .colour = hog::formation_lab::rgb(220, 110, 50)},
    {.name = "bog iron",
     .field = &hog::formation::Ores::bog_iron,
     .colour = hog::formation_lab::rgb(110, 70, 40)},
    {.name = "lead and zinc",
     .field = &hog::formation::Ores::lead_zinc,
     .colour = hog::formation_lab::rgb(130, 140, 170)},
    {.name = "salt",
     .field = &hog::formation::Ores::salt,
     .colour = hog::formation_lab::rgb(250, 250, 250)},
    {.name = "coal",
     .field = &hog::formation::Ores::coal,
     .colour = hog::formation_lab::rgb(25, 25, 25)},
    {.name = "oil",
     .field = &hog::formation::Ores::oil,
     .colour = hog::formation_lab::rgb(90, 30, 110)},
    {.name = "diamonds",
     .field = &hog::formation::Ores::diamonds,
     .colour = hog::formation_lab::rgb(120, 220, 250)},
}};

// Each point in the colour of its likeliest ore, where one is likely at all.
bool draw_ores(const Canvas& canvas, const hog::formation::Rivers& rivers,
               const hog::formation::Ores& found) {
    const Sphere& sphere = *canvas.sphere;
    std::uint32_t lit = 0;
    return canvas.draw("ores.png", [&](const DVec3& direction, const Sphere::Spot& spot) {
        const std::uint32_t near = sphere.nearest(direction, spot.corners[0]);
        if (rivers.receiver[near] == near) {
            return hog::formation_lab::rgb(20, 30, 60);
        }
        Rgb colour = hog::formation_lab::rgb(110, 120, 100);
        double likeliest = 0.25;
        for (const Ore& ore : ores) {
            const double likely = (found.*ore.field)[near];
            if (likely > likeliest) {
                likeliest = likely;
                colour = ore.colour;
            }
        }
        return hog::formation_lab::scaled(colour, canvas.light(rivers.height, direction, lit));
    });
}

void report_ores(const Sphere& sphere, const hog::formation::Rivers& rivers,
                 const hog::formation::Ores& found) {
    double land = 0.0;
    std::array<double, ores.size()> area{};
    for (std::uint32_t point = 0; point < sphere.size(); ++point) {
        if (rivers.receiver[point] == point) {
            continue;
        }
        land += sphere.area(point);
        for (std::size_t ore = 0; ore < ores.size(); ++ore) {
            area.at(ore) += (found.*ores.at(ore).field)[point] > 0.3 ? sphere.area(point) : 0.0;
        }
    }
    std::string line = "ores likely on the land:";
    for (std::size_t ore = 0; ore < ores.size(); ++ore) {
        line += std::format(" {} {:.1f}% (best {:.2f}){}", ores.at(ore).name,
                            100.0 * area.at(ore) / std::max(land, 1e-12),
                            std::ranges::max(found.*ores.at(ore).field),
                            ore + 1 < ores.size() ? "," : "");
    }
    std::cout << line << '\n';
}

void report_bedrock(const Sphere& sphere, const hog::formation::Rivers& rivers,
                    const hog::formation::Bedrock& bedrock,
                    const hog::formation::Outcrop& outcrop) {
    std::array<double, 5> area{};
    double land = 0.0;
    for (std::uint32_t point = 0; point < sphere.size(); ++point) {
        if (rivers.receiver[point] != point) {
            land += sphere.area(point);
            area.at(static_cast<std::size_t>(outcrop.rock[point])) += sphere.area(point);
        }
    }
    const auto share = [&](hog::formation::Rock rock) {
        return 100.0 * area.at(static_cast<std::size_t>(rock)) / std::max(land, 1e-12);
    };
    using hog::formation::Rock;
    std::cout << std::format("land's rock: lava {:.0f}%, layered {:.0f}%, folded {:.0f}%, "
                             "basement {:.0f}%; layered rock up to {:.0f} m, lava up to {:.0f} m\n",
                             share(Rock::Lava), share(Rock::Layered), share(Rock::Folded),
                             share(Rock::Basement), *std::ranges::max_element(bedrock.cover),
                             *std::ranges::max_element(bedrock.lava));
}

// Laid-down sediment: on land from none (rock and soil only) through tan to dark, thicker darker;
// on the sea floor, deltas pale over the deep blue.
// Where ice laid more than rivers did, its till is drawn grey-blue instead.
bool draw_sediment(const Canvas& canvas, const hog::formation::Rivers& rivers,
                   const hog::formation::Ice& ice, const hog::formation::Sediment& sediment) {
    const Sphere& sphere = *canvas.sphere;
    static constexpr std::array<Rgb, 5> thin_to_thick{
        hog::formation_lab::rgb(120, 120, 110), hog::formation_lab::rgb(200, 180, 120),
        hog::formation_lab::rgb(160, 120, 60), hog::formation_lab::rgb(110, 75, 40),
        hog::formation_lab::rgb(60, 40, 25)};
    static constexpr std::array<Rgb, 4> till{
        hog::formation_lab::rgb(120, 120, 110), hog::formation_lab::rgb(160, 175, 185),
        hog::formation_lab::rgb(100, 120, 150), hog::formation_lab::rgb(50, 65, 100)};
    static constexpr std::array<Rgb, 3> delta{hog::formation_lab::rgb(15, 25, 55),
                                              hog::formation_lab::rgb(60, 90, 120),
                                              hog::formation_lab::rgb(190, 180, 140)};
    std::uint32_t lit = 0;
    return canvas.draw("sediment.png", [&](const DVec3& direction, const Sphere::Spot& spot) {
        const std::uint32_t near = sphere.nearest(direction, spot.corners[0]);
        // Thicknesses run from centimetres to kilometres, so they're shown by tens.
        const auto tens = [](double metres) { return std::log10(1.0 + std::max(metres, 0.0)); };
        if (rivers.receiver[near] == near) {
            return hog::formation_lab::ramp(tens(sediment.delta[near] + ice.till[near]) / 3.0,
                                            delta);
        }
        const double light = canvas.light(rivers.height, direction, lit);
        if (ice.till[near] > sediment.alluvium[near]) {
            return hog::formation_lab::scaled(
                hog::formation_lab::ramp(tens(ice.till[near]) / 3.0, till), light);
        }
        return hog::formation_lab::scaled(
            hog::formation_lab::ramp(tens(sediment.alluvium[near]) / 3.0, thin_to_thick), light);
    });
}

// Soil from bare grey through tan to dark, deeper darker; ice, standing water and salt drawn over
// it.
bool draw_soil(const Canvas& canvas, const hog::formation::Rivers& rivers,
               const hog::formation::Ice& ice, const hog::formation::Lakes& lakes,
               const hog::formation::Sediment& sediment, const hog::formation::Soil& soil) {
    const Sphere& sphere = *canvas.sphere;
    static constexpr std::array<Rgb, 5> bare_to_deep{
        hog::formation_lab::rgb(150, 150, 150), hog::formation_lab::rgb(210, 190, 140),
        hog::formation_lab::rgb(170, 120, 70), hog::formation_lab::rgb(110, 70, 40),
        hog::formation_lab::rgb(60, 35, 20)};
    std::uint32_t lit = 0;
    return canvas.draw("soil.png", [&](const DVec3& direction, const Sphere::Spot& spot) {
        const std::uint32_t near = sphere.nearest(direction, spot.corners[0]);
        if (rivers.receiver[near] == near) {
            return hog::formation_lab::rgb(20, 30, 60);
        }
        const double light = canvas.light(rivers.height, direction, lit);
        if (ice.thickness[near] > 0.0) {
            return hog::formation_lab::scaled(ice_white, light);
        }
        if (const auto water = water_colour(lakes, sediment, near)) {
            return *water;
        }
        const double depth = sphere.sample(soil.depth, spot);
        return hog::formation_lab::scaled(hog::formation_lab::ramp(depth / 2.5, bare_to_deep),
                                          light);
    });
}

// What lies on top: peat purple-black by thickness over 30 cm, otherwise topsoil from pale (none)
// to near black; ice, standing water and salt drawn over it.
bool draw_topsoil(const Canvas& canvas, const hog::formation::Rivers& rivers,
                  const hog::formation::Ice& ice, const hog::formation::Lakes& lakes,
                  const hog::formation::Sediment& sediment, const hog::formation::Soil& soil) {
    const Sphere& sphere = *canvas.sphere;
    static constexpr std::array<Rgb, 4> pale_to_dark{
        hog::formation_lab::rgb(200, 185, 150), hog::formation_lab::rgb(150, 115, 70),
        hog::formation_lab::rgb(85, 60, 35), hog::formation_lab::rgb(35, 25, 15)};
    static constexpr std::array<Rgb, 3> peat{hog::formation_lab::rgb(90, 70, 70),
                                             hog::formation_lab::rgb(60, 35, 55),
                                             hog::formation_lab::rgb(25, 10, 25)};
    std::uint32_t lit = 0;
    return canvas.draw("topsoil.png", [&](const DVec3& direction, const Sphere::Spot& spot) {
        const std::uint32_t near = sphere.nearest(direction, spot.corners[0]);
        if (rivers.receiver[near] == near) {
            return hog::formation_lab::rgb(20, 30, 60);
        }
        const double light = canvas.light(rivers.height, direction, lit);
        if (ice.thickness[near] > 0.0) {
            return hog::formation_lab::scaled(ice_white, light);
        }
        if (const auto water = water_colour(lakes, sediment, near)) {
            return *water;
        }
        const double organic = sphere.sample(soil.organic, spot);
        if (organic > 0.3) {
            return hog::formation_lab::scaled(hog::formation_lab::ramp((organic - 0.3) / 1.5, peat),
                                              light);
        }
        const double topsoil = sphere.sample(soil.topsoil, spot);
        return hog::formation_lab::scaled(hog::formation_lab::ramp(topsoil / 0.5, pale_to_dark),
                                          light);
    });
}

// Land area soiled, and mean soil depth on each parent where soil forms at all.
void report_soil(const Sphere& sphere, const hog::formation::Crust& crust,
                 const hog::formation::Soil& soil) {
    using hog::formation::Parent;
    double land = 0.0;
    double soiled = 0.0;
    std::array<double, 7> area{};
    std::array<double, 7> depth{};
    double young = 0.0;
    double young_depth = 0.0;
    for (std::uint32_t point = 0; point < sphere.size(); ++point) {
        if (crust.sea[point] != 0) {
            continue;
        }
        const double here = sphere.area(point);
        land += here;
        soiled += soil.depth[point] > 0.0 ? here : 0.0;
        const auto kind = static_cast<std::size_t>(soil.parent[point]);
        area.at(kind) += here;
        depth.at(kind) += soil.depth[point] * here;
        if (soil.parent[point] != Parent::None && soil.years[point] < 1.0e5) {
            young += here;
            young_depth += soil.depth[point] * here;
        }
    }
    const auto mean = [&](Parent parent) {
        const auto kind = static_cast<std::size_t>(parent);
        return depth.at(kind) / std::max(area.at(kind), 1e-12);
    };
    const auto share = [&](Parent parent) {
        return 100.0 * area.at(static_cast<std::size_t>(parent)) / std::max(land, 1e-12);
    };
    std::cout << std::format(
        "soil on {:.0f}% of the land, deepest {:.2f} m; on rock {:.0f}% ({:.2f} m), till {:.1f}% "
        "({:.2f} m), alluvium {:.0f}% ({:.2f} m), lake beds {:.1f}% ({:.2f} m), loess {:.1f}% "
        "({:.2f} m), sand {:.1f}% ({:.2f} m); where ice left {:.0f}% ({:.2f} m)\n",
        100.0 * soiled / std::max(land, 1e-12), *std::ranges::max_element(soil.depth),
        share(Parent::Rock), mean(Parent::Rock), share(Parent::Till), mean(Parent::Till),
        share(Parent::Alluvium), mean(Parent::Alluvium), share(Parent::LakeBed),
        mean(Parent::LakeBed), share(Parent::Loess), mean(Parent::Loess), share(Parent::Sand),
        mean(Parent::Sand), 100.0 * young / std::max(land, 1e-12),
        young_depth / std::max(young, 1e-12));
    double rotted = 0.0;
    for (std::uint32_t point = 0; point < sphere.size(); ++point) {
        rotted += crust.sea[point] == 0 && soil.saprolite[point] > 10.0 ? sphere.area(point) : 0.0;
    }
    std::cout << std::format(
        "under the soil, deepest alluvium {:.0f} m, lake beds {:.0f} m, "
        "till {:.0f} m; rock rotted over 10 m deep on {:.0f}% of the land, "
        "deepest {:.0f} m\n",
        *std::ranges::max_element(soil.alluvium), *std::ranges::max_element(soil.lake_beds),
        *std::ranges::max_element(soil.till), 100.0 * rotted / std::max(land, 1e-12),
        *std::ranges::max_element(soil.saprolite));
    double dark = 0.0;
    double topsoil = 0.0;
    double peat = 0.0;
    for (std::uint32_t point = 0; point < sphere.size(); ++point) {
        if (crust.sea[point] == 0 && soil.topsoil[point] > 0.0) {
            dark += sphere.area(point);
            topsoil += soil.topsoil[point] * sphere.area(point);
        }
        peat += crust.sea[point] == 0 && soil.organic[point] > 0.3 ? sphere.area(point) : 0.0;
    }
    std::cout << std::format(
        "topsoil on {:.0f}% of the land, {:.2f} m on average, deepest "
        "{:.2f} m; litter or peat deepest {:.2f} m, peat over 30 cm on {:.1f}% "
        "of the land\n",
        100.0 * dark / std::max(land, 1e-12), topsoil / std::max(dark, 1e-12),
        *std::ranges::max_element(soil.topsoil), *std::ranges::max_element(soil.organic),
        100.0 * peat / std::max(land, 1e-12));
}

// Wind-laid sand gold, deeper redder, with dune fields bright; loess pale olive by depth.
bool draw_drifts(const Canvas& canvas, const hog::formation::Rivers& rivers,
                 const hog::formation::Ice& ice, const hog::formation::Lakes& lakes,
                 const hog::formation::Sediment& sediment, const hog::formation::Drifts& drifts) {
    const Sphere& sphere = *canvas.sphere;
    static constexpr std::array<Rgb, 3> sand{hog::formation_lab::rgb(205, 180, 120),
                                             hog::formation_lab::rgb(220, 150, 70),
                                             hog::formation_lab::rgb(190, 90, 40)};
    static constexpr std::array<Rgb, 3> loess{hog::formation_lab::rgb(120, 120, 110),
                                              hog::formation_lab::rgb(175, 165, 110),
                                              hog::formation_lab::rgb(215, 200, 140)};
    std::uint32_t lit = 0;
    return canvas.draw("drifts.png", [&](const DVec3& direction, const Sphere::Spot& spot) {
        const std::uint32_t near = sphere.nearest(direction, spot.corners[0]);
        if (rivers.receiver[near] == near) {
            return hog::formation_lab::rgb(20, 30, 60);
        }
        const double light = canvas.light(rivers.height, direction, lit);
        if (ice.thickness[near] > 0.0) {
            return hog::formation_lab::scaled(ice_white, light);
        }
        if (const auto water = water_colour(lakes, sediment, near)) {
            return *water;
        }
        const double laid = sphere.sample(drifts.sand, spot);
        if (laid > 1.0) {
            const double dunes = sphere.sample(drifts.dunes, spot);
            return hog::formation_lab::scaled(
                hog::formation_lab::ramp(std::max(laid / 60.0, dunes), sand), light);
        }
        return hog::formation_lab::scaled(
            hog::formation_lab::ramp(sphere.sample(drifts.loess, spot) / 30.0, loess), light);
    });
}

// How much land the wind strips and covers, and how deep.
void report_drifts(const Sphere& sphere, const hog::formation::Crust& crust,
                   const hog::formation::Drifts& drifts) {
    double land = 0.0;
    double stripped = 0.0;
    double sanded = 0.0;
    double duned = 0.0;
    double loessed = 0.0;
    double loess_depth = 0.0;
    for (std::uint32_t point = 0; point < sphere.size(); ++point) {
        if (crust.sea[point] != 0) {
            continue;
        }
        const double here = sphere.area(point);
        land += here;
        stripped += drifts.deflated[point] > 1.0 ? here : 0.0;
        sanded += drifts.sand[point] > 1.0 ? here : 0.0;
        duned += drifts.dunes[point] > 0.5 ? here : 0.0;
        if (drifts.loess[point] > 1.0) {
            loessed += here;
            loess_depth += drifts.loess[point] * here;
        }
    }
    const auto share = [&](double area) { return 100.0 * area / std::max(land, 1e-12); };
    std::cout << std::format(
        "wind: stripped over 1 m on {:.1f}% of the land, deepest {:.0f} m; sand over 1 m on "
        "{:.1f}%, deepest {:.0f} m, dune fields {:.1f}%; loess over 1 m on {:.1f}% ({:.1f} m), "
        "deepest {:.0f} m\n",
        share(stripped), *std::ranges::max_element(drifts.deflated), share(sanded),
        *std::ranges::max_element(drifts.sand), share(duned), share(loessed),
        loess_depth / std::max(loessed, 1e-12), *std::ranges::max_element(drifts.loess));
}

void report_ice(const Sphere& sphere, const hog::formation::Crust& crust,
                const hog::formation::Rivers& rivers, const hog::formation::Ice& ice,
                double seconds) {
    double land = 0.0;
    double now = 0.0;
    double coldest = 0.0;
    int fjords = 0;
    for (std::uint32_t point = 0; point < sphere.size(); ++point) {
        if (crust.sea[point] != 0) {
            continue;
        }
        land += sphere.area(point);
        now += ice.thickness[point] > 0.0 ? sphere.area(point) : 0.0;
        coldest += ice.coldest[point] > 0.0 ? sphere.area(point) : 0.0;
        fjords += rivers.receiver[point] == point ? 1 : 0;
    }
    land = std::max(land, 1e-12);
    std::cout << std::format(
        "ice in {:.2f} s: on {:.0f}% of the land now, {:.0f}% in the coldest times; thickest "
        "{:.0f} m, deepest carving {:.0f} m, {} points of sea let into carved troughs; "
        "{:.0f} km³ of water a year breaks off into the sea\n",
        seconds, 100.0 * now / land, 100.0 * coldest / land,
        *std::ranges::max_element(ice.thickness), *std::ranges::max_element(ice.carved), fjords,
        ice.calving / 1e9);
    double tilled = 0.0;
    double laid = 0.0;
    for (std::uint32_t point = 0; point < sphere.size(); ++point) {
        if (crust.sea[point] == 0 && ice.till[point] > 0.0) {
            tilled += sphere.area(point);
            laid += ice.till[point] * sphere.area(point);
        }
    }
    std::cout << std::format("till on {:.0f}% of the land, {:.0f} m deep on average there, "
                             "thickest {:.0f} m\n",
                             100.0 * tilled / land, laid / std::max(tilled, 1e-12),
                             *std::ranges::max_element(ice.till));
}

int run(const Options& options) {
    const hog::formation::Body& body = options.body;
    auto at = std::chrono::steady_clock::now();
    const Sphere layout{options.layout, body.seed};
    const Sphere sphere{options.detail, body.seed + 1};
    std::cout << std::format("spheres: {} and {} points in {:.2f} s\n", layout.size(),
                             sphere.size(), seconds_since(at));
    std::filesystem::create_directories(options.out);
    const Canvas canvas{
        .options = &options, .sphere = &sphere, .step = std::numbers::pi / (options.width / 2.0)};

    at = std::chrono::steady_clock::now();
    const hog::formation::Plates plates{body, layout};
    const hog::formation::Crust crust = hog::formation::crust_on(sphere, plates, body);
    std::cout << std::format("plates and crust in {:.2f} s\n", seconds_since(at));
    bool ok = draw_crust(canvas, crust);

    at = std::chrono::steady_clock::now();
    const hog::formation::Climate climate =
        hog::formation::climate_on(sphere, crust.base, crust.sea, body);
    std::cout << std::format("climate in {:.2f} s\n", seconds_since(at));
    ok = draw_climate(canvas, crust, climate) && ok;

    at = std::chrono::steady_clock::now();
    const hog::formation::Bedrock bedrock = hog::formation::bedrock_on(sphere, crust, body);
    const hog::formation::Rivers worn = hog::formation::rivers_on(
        sphere, crust, climate, hog::formation::standing_strength(sphere, crust, bedrock, body),
        body);
    std::cout << std::format("rivers in {:.2f} s, settled after {} routings, highest {:.0f} m\n",
                             seconds_since(at), worn.rounds,
                             *std::ranges::max_element(worn.height));
    at = std::chrono::steady_clock::now();
    const hog::formation::Ice ice = hog::formation::ice_on(sphere, crust, climate, worn, body);
    const hog::formation::Rivers rivers = hog::formation::ice_shaped(sphere, worn, ice);
    report_ice(sphere, crust, rivers, ice, seconds_since(at));
    ok = draw_ice(canvas, crust, rivers, ice) && ok;

    const hog::formation::Outcrop outcrop =
        hog::formation::outcrop_on(sphere, crust, bedrock, rivers, body);
    report_bedrock(sphere, rivers, bedrock, outcrop);
    ok = draw_bedrock(canvas, rivers, bedrock, outcrop) && ok;
    const hog::formation::Landforms landforms =
        hog::formation::landforms_on(sphere, crust, bedrock, outcrop, climate, rivers, ice, body);
    report_landforms(sphere, rivers, landforms);
    ok = draw_landforms(canvas, rivers, landforms) && ok;

    const hog::formation::Lakes lakes = hog::formation::lakes_on(sphere, rivers, climate, body);
    const auto open =
        std::ranges::count_if(lakes.lakes, [](const auto& lake) { return lake.open; });
    std::cout << std::format("{} lakes, {} open and {} closed; {} salt flats\n", lakes.lakes.size(),
                             open, static_cast<std::ptrdiff_t>(lakes.lakes.size()) - open,
                             std::ranges::count(lakes.salt_flat, std::uint8_t{1}));
    const hog::formation::Sediment sediment =
        hog::formation::sediment_on(sphere, rivers, lakes, ice.outwash, body);
    std::vector<std::uint8_t> still_wet(lakes.lakes.size(), 0);
    for (std::uint32_t point = 0; point < sphere.size(); ++point) {
        if (lakes.lake[point] != hog::formation::Lakes::none && sediment.lake_depth[point] > 0.0) {
            still_wet[lakes.lake[point]] = 1;
        }
    }
    const auto filled = std::ranges::count(still_wet, std::uint8_t{0});
    std::cout << std::format("sediment: deepest {:.0f} m on land, {:.0f} m at river mouths; "
                             "{} lakes filled in\n",
                             *std::ranges::max_element(sediment.alluvium),
                             *std::ranges::max_element(sediment.delta), filled);
    ok = draw_rivers(canvas, rivers, ice, lakes, sediment) && ok;
    ok = draw_sediment(canvas, rivers, ice, sediment) && ok;

    const hog::formation::Drifts drifts =
        hog::formation::drifts_on(sphere, climate, rivers, ice, lakes, sediment, body);
    report_drifts(sphere, crust, drifts);
    ok = draw_drifts(canvas, rivers, ice, lakes, sediment, drifts) && ok;

    const hog::formation::Soil soil =
        hog::formation::soil_on(sphere, rivers, climate, ice, lakes, sediment, drifts, body);
    report_soil(sphere, crust, soil);
    ok = draw_soil(canvas, rivers, ice, lakes, sediment, soil) && ok;
    ok = draw_topsoil(canvas, rivers, ice, lakes, sediment, soil) && ok;

    const hog::formation::Ores found = hog::formation::ores_on(
        sphere, crust, bedrock, outcrop, climate, rivers, lakes, sediment, soil, body);
    report_ores(sphere, rivers, found);
    ok = draw_ores(canvas, rivers, found) && ok;
    return ok ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    try {
        Options options;
        const std::span<char*> arguments{argv, static_cast<std::size_t>(argc)};
        for (std::size_t n = 1; n < arguments.size(); n += 2) {
            const std::string_view name = arguments[n];
            if (name == "--help" || n + 1 >= arguments.size() ||
                !apply(options, name, arguments[n + 1])) {
                std::cerr << usage;
                return name == "--help" ? 0 : 2;
            }
        }
        return run(options);
    } catch (const std::exception& error) {
        std::cerr << "hog-formation: " << error.what() << "\n";
        return 1;
    }
}
