#pragma once

#include <hog/flow/flight.hpp>
#include <hog/flow/slump.hpp>
#include <hog/globe/brick.hpp>
#include <hog/globe/cell.hpp>
#include <hog/globe/material.hpp>
#include <hog/globe/surface.hpp>
#include <hog/globe/voxel.hpp>
#include <hog/globe/world.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <unordered_map>
#include <vector>

namespace hog::flow {

// What a step did: the voxels' amounts' changes summed, how much the sea beyond the edge gave,
// how much shaped ground the current wore loose, and the bricks not held yet that liquid stands
// against and would move into.
struct Stepped {
    std::int64_t changed = 0;
    std::int64_t from_sea = 0;
    std::int64_t eroded = 0;
    // Grain that slid down a heap too steep to hold, summed over the step's passes.
    std::int64_t slumped = 0;
    // Lumps of grain in motion at the end of the step, and the grain that found no ground to
    // land on and was lost.
    std::size_t flying = 0;
    std::int64_t scattered = 0;
    // Voxels of shaped matter that lost their hold and fell this step.
    std::int64_t collapsed = 0;
    // Voxels of lava that cooled and set into rock this step.
    std::int64_t frozen = 0;
    // Units of water boiled off by lava it touched.
    std::int64_t steamed = 0;
    std::vector<globe::BrickSite> wanted;
    // Each column with liquid standing in the open, and how much.
    struct Wet {
        std::uint8_t face = 0;
        std::uint32_t i = 0;
        std::uint32_t j = 0;
        std::int64_t standing = 0;
    };
    std::vector<Wet> wet;
};

// Liquid moving through a world's voxels, over as many of its bricks as are held here. Each
// column's open voxels between the shaped matter make its sections; a section's liquid settles
// to its bottom, and where two neighbouring sections share a layer and one stands higher, liquid
// runs across into the other by the difference in their heads. Liquid pushed into a section
// already full presses in beyond full, and the head that gives pushes on in turn, so liquid finds
// its level through a tunnel too. The columns are square and so is the flow: across the four
// sides alike. Standing liquid seeps into the ground under it as fast as the ground lets it,
// and water in the ground drains down through it, keeping back what the ground holds.
//
// A current drags on its bed; where the drag exceeds what the bed withstands it wears the bed
// away, grain piled on it first, then the shaped ground under it, whose surface it pushes in
// and which comes open when its middle is reached. What it wears off it carries along with the
// liquid it moves, and the carried load sinks back out at its settling speed onto the bed below,
// as grain piled in the bottom voxel, which takes room from the liquid and is ground to the
// flow once it fills its voxel. The load is the region's: it is lost with a brick let go.
class Region {
public:
    // With `hold_sea`, open voxels under the sea's level at the edge of what is held are kept
    // full, the sea beyond being endless.
    explicit Region(globe::World& world, bool hold_sea = true);

    // Reads the brick as the world now has it, unless held already.
    void load(globe::BrickSite site);
    // Lets the brick go; what it held unflushed is lost.
    void unload(globe::BrickSite site);
    [[nodiscard]] bool holds(globe::BrickSite site) const;
    [[nodiscard]] std::size_t size() const noexcept { return held_.size(); }

    // Precondition: its brick is held.
    [[nodiscard]] globe::Voxel at(globe::Site site) const;
    // Sets the voxel to `amount` of `rock` molten at `kelvin`.
    void pour_lava(globe::Site site, globe::Material rock, std::uint16_t amount, double kelvin);
    // Adds up to `units` of `rock` molten at `kelvin` to the voxel, as much as fits; how much
    // went in.
    std::int64_t add_lava(globe::Site site, globe::Material rock, std::int64_t units, double kelvin);
    // Whether the brick has lava in it, which keeps cooling for as long as it does.
    [[nodiscard]] bool hot(globe::BrickSite site) const;
    // Sets the voxel's liquid to `amount` of the world's own.
    void pour(globe::Site site, std::uint16_t amount);
    // Sets the voxel's grain, piled on whatever is under it.
    void pile(globe::Site site, globe::Material material, std::uint16_t amount);
    // Adds `amount` of `material` to what the liquid at the site carries.
    void suspend(globe::Site site, globe::Material material, std::int64_t amount);
    // All the liquid held, in the open and in the ground's pores.
    [[nodiscard]] std::int64_t liquid() const;
    // All the loose matter held: grain piled in voxels, in flight and carried by the liquid.
    [[nodiscard]] std::int64_t grain() const;
    // The grain in motion, which is the region's until it lands. It falls over a drop and bounces
    // or rolls on, a step at a time, so whoever shows it can show it travelling.
    [[nodiscard]] const std::vector<Parcel>& parcels() const noexcept { return parcels_; }
    // Sends a lump of grain flying from `at` with `velocity`.
    void throw_grain(DVec3 at, DVec3 velocity, globe::Material material, std::int64_t amount);

    // How steep a slope a rolling boulder runs down, as a rise per run: gentler than what holds
    // a heap, as a block rolls more freely than grain slides.
    static constexpr double rolls_down = 0.2;
    // Takes a fallen rock out of the ground and sends it rolling from where it lay, as a
    // boulder among the parcels. Precondition: the bricks it stands in are held.
    void loosen(const globe::Rock& rock);
    // Frees each rock generated about `point` that is still there and has nothing under it. The
    // number it couldn't judge for want of bricks held.
    std::size_t loosen_unsupported(DVec3 point, std::size_t& unjudged);

    // Seconds a matter that has lost its hold takes to crack through, from whole to falling.
    static constexpr double cracks_in = 3.0;
    // Bricks looked at for support in one step, at most.
    static constexpr std::size_t most_inspected = 4;
    // Queues the brick, and the matter about it, to be looked at for support: it is cracked and
    // let fall where it hangs from nothing. Wanted bricks about it are asked for.
    // `weathered` is game seconds of weather and rust and the like to wear the brick by when it is
    // looked at, which is what the slow sweep brings on its rounds.
    void disturb(globe::BrickSite site, double weathered = 0.0);
    // Bricks waiting to be looked at or cracking.
    [[nodiscard]] std::size_t inspecting() const noexcept { return inspect_.size(); }

    // What the liquid up a column carries.
    struct Carried {
        globe::Material material = globe::Material::Air;
        std::int64_t amount = 0;
    };
    [[nodiscard]] Carried carried(std::uint8_t face, std::uint32_t i, std::uint32_t j) const;

    // Runs the liquid `seconds` on.
    Stepped step(double seconds);
    // Writes the bricks changed since they were loaded or last flushed, and says which.
    std::vector<globe::BrickSite> flush();

    // The liquid the last step carried out of a column across its +i and +j sides, less what
    // came in across them: the current, for wheels, boats and what the water carries.
    struct Flux {
        std::int64_t along_i = 0;
        std::int64_t along_j = 0;
    };
    [[nodiscard]] Flux flux(std::uint8_t face, std::uint32_t i, std::uint32_t j) const;

private:
    struct Held {
        globe::BrickVoxels voxels{};
        bool dirty = false;
    };
    // A run of open voxels up a column, `bottom` up to but not `top`, the liquid in it, the
    // grain piled on its bed and the load the liquid carries.
    struct Section {
        int bottom = 0;
        int top = 0;
        std::int64_t amount = 0;
        globe::Material liquid = globe::Material::Air;
        std::int64_t heat = 0; // of a molten liquid: its amount times its temperature, in eighths
        bool moving = false; // liquid ran in or out of it this step
        std::int64_t bed = 0;
        std::int64_t load = 0;
        globe::Material carried = globe::Material::Air;
        std::int64_t fall = 0; // the steepest head difference liquid ran across this step
    };
    // What a column's liquid carries, kept between steps by the layer it was carried at.
    struct Load {
        std::int64_t amount = 0;
        globe::Material material = globe::Material::Air;
        std::uint32_t layer = 0;
    };
    struct Column {
        std::uint8_t face = 0;
        std::uint32_t i = 0;
        std::uint32_t j = 0;
        std::size_t first = 0; // of its sections
        std::size_t count = 0;
    };
    // Where two sections meet: open at the same layers, in columns side by side.
    struct Meeting {
        std::size_t here = 0;
        std::size_t there = 0;
        int window = 0; // the lowest layer both are open at
        std::size_t column = 0;
        bool along_i = true; // `there` is across its +i side, else its +j side
    };
    struct Transfer {
        std::size_t from = 0;
        std::size_t to = 0;
        std::int64_t amount = 0;
        std::int64_t fall = 0;  // the head difference driving it
        std::size_t column = 0; // whose +i or +j side it crosses
        bool along_i = true;
        bool outward = true; // out of `column`
    };

    [[nodiscard]] Held* held(globe::BrickSite site);
    [[nodiscard]] const Held* held(globe::BrickSite site) const;
    [[nodiscard]] globe::Voxel* voxel(globe::Site site);
    // The site `di` and `dj` across from it on its face, or none past the face's edge.
    [[nodiscard]] std::optional<globe::Site> beside(globe::Site site, int di, int dj) const;
    [[nodiscard]] bool open_to_sea(globe::Site site) const;
    // Whether any of the four voxels beside it isn't held.
    [[nodiscard]] bool at_edge(globe::Site site);

    [[nodiscard]] std::vector<Column> columns() const;
    // Calls `each(site, brick, voxel)` up the column, open voxel after open voxel, and `gap()`
    // where a run ends.
    template <typename Each, typename Gap>
    void walk(const Column& column, const Each& each, const Gap& gap);
    // Calls `each(site, brick, voxel, joined)` up the column for every voxel held, `joined` when
    // the one under it was held too.
    template <typename Each>
    void walk_all(const Column& column, const Each& each);
    void hold_sea(const std::vector<Column>& columns, Stepped& stepped);
    void gather(std::vector<Column>& columns);
    [[nodiscard]] std::vector<Meeting> meetings(const std::vector<Column>& columns) const;
    // Adds where the sections of two columns side by side meet, `there` across `here`'s +i or
    // +j side.
    void meet(std::size_t c, const Column& here, const Column& there, bool along_i,
              std::vector<Meeting>& meetings) const;
    // Each section's head: its surface, or what pushes on it when full.
    [[nodiscard]] std::vector<std::int64_t> heads(const std::vector<Meeting>& meetings) const;
    [[nodiscard]] std::vector<Transfer> transfers(const std::vector<Column>& columns) const;
    void want_around(globe::Site site, std::set<std::uint64_t>& wanted);
    // Scales the transfers at each section's `end` down to `limit(section)` where `asked` of it
    // exceeds that.
    static void scale(std::vector<Transfer>& transfers, std::size_t Transfer::* end,
                      const std::vector<std::int64_t>& asked, const auto& limit);
    void settle(const std::vector<Column>& columns, double seconds, Stepped& stepped);
    // Where water touches lava the lava chills and the water boils away: lava that has water
    // enough sets into rock, else it cools by what there is.
    void quench(Stepped& stepped);
    // Sets a voxel of cooled lava into rock, bringing the open voxels about it up to its surface.
    void freeze(globe::Site site, Held& brick);
    void report_wet(const Column& column, Stepped& stepped) const;
    void seep(const std::vector<Column>& columns, double seconds, Stepped& stepped);
    // Seeps from a voxel into the one under it; how much moved.
    [[nodiscard]] std::int64_t seep_down(globe::Site site, globe::Voxel& over, globe::Voxel& under,
                                         double seconds) const;
    // `value` rounded to whole units, the fraction by a hash of the site, the step and `salt`,
    // so what moves slowly still adds up.
    [[nodiscard]] std::int64_t dithered(double value, globe::Site site, std::uint64_t salt) const;
    // Units of liquid a seepage of `rate` metres a second moves in `seconds`.
    [[nodiscard]] std::int64_t seeped(double rate, double seconds, globe::Site site) const;

    // Gives each section the load its column carried, dropping a load whose liquid is gone.
    void take_up_loads(const std::vector<Column>& columns);
    // Each section's current wears its bed and carries what it wears off, and its load settles
    // out onto the bed.
    void carry(const std::vector<Column>& columns, double seconds, Stepped& stepped);
    // The bottom voxel of a section, the one under it if held, and the brick each is in.
    struct Bed {
        globe::Site site;
        Held* brick = nullptr;
        globe::Voxel* bottom = nullptr;
        globe::Voxel* under = nullptr;
        Held* under_brick = nullptr;
    };
    void wear(Section& section, const Bed& bed, double shear, double seconds, Stepped& stepped);
    void drop(Section& section, const Bed& bed, double depth, double seconds, Stepped& stepped);
    // Pushes the shaped surface under the bed voxel `mm` further in, so far as the voxels about
    // it tell that surface's distance.
    void scour(globe::Site bed, int mm);
    void keep_loads(const std::vector<Column>& columns);
    // Grain on the topmost ground slides to lower columns beside it where it lies steeper than
    // its repose; see Slump. Only the top surface of a column slumps, not floors inside caves.
    void slump(Stepped& stepped);
    // A column's top ground, the grain on it, and the ground's height in units where
    // Voxel::full is a voxel's height, above the floor. Looks down from layer `start`.
    struct Scan {
        globe::Surface surface;
        std::int64_t ground = 0;
    };
    [[nodiscard]] std::optional<Scan> scan(std::uint8_t face, std::uint32_t i, std::uint32_t j,
                                           std::uint32_t start, std::uint32_t reach);
    // The floors up a column from layer `low` to `high`: each run of open voxels over shaped
    // matter. Grain with open space under it in one is let fall, as lumps in flight.
    [[nodiscard]] std::vector<Scan> floors(std::uint8_t face, std::uint32_t i, std::uint32_t j,
                                           std::uint32_t low, std::uint32_t high);
    bool drop_hanging(std::uint8_t face, std::uint32_t i, std::uint32_t j, const Scan& floor);
    // Sets the grain over a column's ground to `depth`, filling its voxels from `column`'s layer
    // up, `voxels` of them.
    void fill_grain(globe::Site column, std::uint32_t voxels, std::int64_t depth,
                    globe::Material material);
    // The surface a parcel at `at` would meet, or none where nothing here is held to land on.
    [[nodiscard]] std::optional<Ground> ground_under(DVec3 at);
    [[nodiscard]] DVec3 heap_normal(const globe::Cell& column, std::uint32_t layer, DVec3 fallback);
    // The brick of the first voxel down from `at` that isn't held, if the ground is beyond it.
    [[nodiscard]] std::optional<globe::BrickSite> unheld_below(DVec3 at);
    // Moves each parcel on `seconds`, and lets the grain of those that came to rest settle into
    // the column under them.
    void fly(double seconds, Stepped& stepped);
    // Piles a parcel into the column under it; whether it found a place.
    [[nodiscard]] bool land(const Parcel& parcel, Stepped& stepped);
    // Builds a boulder at rest into the ground as shaped rock; the first brick it needs that
    // isn't held, if any, and then nothing is built.
    [[nodiscard]] std::optional<globe::BrickSite> settle_boulder(const Parcel& parcel);
    // Whether the voxel at `point` is shaped matter; none where its brick isn't held.
    [[nodiscard]] std::optional<bool> solid_at(DVec3 point);
    // Applies `change(voxel, distance, outward)` to each held voxel within the shape's band, and
    // the first brick it covers that isn't held, if any.
    using Distance = std::function<double(DVec3, DVec3&)>;
    using Change = std::function<void(globe::Voxel&, double, DVec3)>;
    std::optional<globe::BrickSite> reshape(DVec3 centre, double reach, const Distance& distance,
                                            const Change& change);
    // Bricks newly held, whose fallen rocks are looked at once their surroundings are held too.
    struct Fresh {
        globe::BrickSite site;
        int tries = 0;
        double weathered = 0.0;
    };
    void loosen_fresh();
    // Looks at the bricks queued for support; those still cracking stay queued.
    void inspect_support(double seconds, Stepped& stepped);
    // Wears the voxels of a brick by `seconds` of weather; those worn to nothing are returned
    // as falling.
    [[nodiscard]] std::vector<globe::Site> weather(globe::BrickSite site, double seconds);
    // Looks at the box of bricks about one; whether any matter in it is cracking.
    [[nodiscard]] bool check_support(globe::BrickSite site, double seconds, Stepped& stepped);
    // Takes the voxels out of the ground and sends them down as boulders and rubble.
    void fall(const std::vector<globe::Site>& sites, Stepped& stepped);

    globe::World* world_;
    bool hold_sea_;
    // Voxels of lava that cooled through this step, to be set into rock at its end.
    std::vector<std::pair<globe::Site, Held*>> setting_;
    globe::Material liquid_;
    std::unordered_map<std::uint64_t, Held> held_;
    // The brick layers held up each column of bricks, in order.
    std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> stacks_;
    std::vector<Section> sections_;
    std::unordered_map<std::uint64_t, std::size_t> column_at_; // into columns(), by its key
    std::unordered_map<std::uint64_t, Flux> flux_;             // the last step's, by column
    std::unordered_map<std::uint64_t, Load> loads_;            // by column
    std::vector<Parcel> parcels_;
    std::vector<Fresh> fresh_;
    std::vector<Fresh> inspect_;
    std::uint64_t weathering_ = 0; // counts the brick weathered, to tell its chances apart
    std::uint32_t parcel_ids_ = 0;
    std::uint64_t steps_ = 0;
};

} // namespace hog::flow
