#pragma once

#include <hog/flow/region.hpp>
#include <hog/flow/vent.hpp>
#include <hog/flow/wetting.hpp>
#include <hog/globe/brick.hpp>
#include <hog/globe/world.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace hog::flow {

// The flow run where the world is touched: an edit wakes the bricks about it, liquid takes up
// the bricks it reaches, and bricks still for a while are let go, so what's at rest costs
// nothing. Each step writes the bricks it changed to the world, which is how anyone hears of it.
class Simulation {
public:
    // Steps a brick goes unchanged before it's let go.
    static constexpr int rest = 60;
    // Bricks taken up in one step, at most; the rest wait their turn.
    static constexpr std::size_t most_taken = 64;

    explicit Simulation(globe::World& world);

    // Wakes the brick and the six about it.
    void touched(globe::BrickSite site);
    // Wakes the brick, and has the matter about it looked at to see it still holds itself up: an
    // edit that took the ground from under something.
    // `weathered` seconds of weather, rust and the like are put on it first.
    void disturbed(globe::BrickSite site, double weathered = 0.0);
    // Looks at the next `count` bricks changed from the generated ground for support, going
    // round them all in turn, so ground left hanging where no one is is brought down in time.
    // Lets time go by for wear alone, as when the world is far ahead of the flow.
    void pass(double seconds) noexcept { clock_ += seconds; }
    // Each brick is worn too, by the time since it was last worn.
    void sweep(std::size_t count);
    // Looks after the bricks held within `reach` metres of `centre` (from the planet's centre),
    // where someone is: those not worn for `gap` seconds are worn by the time since, and checked
    // for support. Far quicker than the sweep comes round.
    void attend(DVec3 centre, double reach, double gap);

    // Sets how ice gains and loses by place; see Region::IceBalance. Ice moves only when aged.
    void set_ice_balance(Region::IceBalance balance) { region_.set_ice_balance(std::move(balance)); }
    // Takes up `bricks` and ages the ice on all the ground held by `years`, however long the flow
    // itself has run: glaciers move over years, not seconds. Returns the bricks written to the
    // world.
    std::vector<globe::BrickSite> age_ice(std::span<const globe::BrickSite> bricks, double years);

    // Ice to lie over a column's top ground.
    struct Laid {
        std::uint8_t face = 0;
        std::uint32_t i = 0;
        std::uint32_t j = 0;
        double metres = 0.0;
    };
    // Lays each column's ice, taking up the ground it needs and letting it go again, and returns
    // the bricks written to the world.
    std::vector<globe::BrickSite> lay_ice(std::span<const Laid> columns);

    // Sets a vent giving lava from the flow's clock on. Its brick is kept held while it gives any.
    void add_vent(const Vent& vent);
    [[nodiscard]] std::size_t vents() const noexcept { return vents_.size(); }

    struct Progress {
        std::int64_t changed = 0;
        std::int64_t eroded = 0;
        std::int64_t slumped = 0;
        std::size_t flying = 0;
        std::int64_t scattered = 0;
        std::int64_t collapsed = 0;
        std::size_t held = 0;
        std::vector<globe::BrickSite> written;
    };
    // Runs the liquid `seconds` on.
    Progress step(double seconds);

    // A column to rain on or dry: its place, with `layer` the height to look for the ground
    // down from, and the sky over it.
    struct Wetted {
        globe::Site column;
        Sky sky;
    };
    // Rains on and dries the ground of `columns` for `seconds`, in the voxels' own liquid, so
    // whoever reads the world reads it wet. Bricks the flow holds are changed there; the rest are
    // written to the world, and a film of water on them wakes them to run off. Returns the bricks
    // written to the world.
    std::vector<globe::BrickSite> wet(std::span<const Wetted> columns, double seconds);

    [[nodiscard]] const Region& region() const noexcept { return region_; }

    // What the flow has left of itself in a column: how much water has stood there, in metres
    // of depth times seconds, and how much has run through, in voxels' worth. Vegetation reads
    // these to grow thick by water and thin away from it.
    struct Moisture {
        double stood = 0.0;
        double ran = 0.0;
    };
    [[nodiscard]] Moisture moisture(std::uint8_t face, std::uint32_t i, std::uint32_t j) const;

private:
    void take(globe::BrickSite site);
    // Pours what the vents give over `seconds`.
    void vent(double seconds);
    // Wears the brick by the time since it last was, if that is at least `gap`.
    void wear(globe::BrickSite site, double gap);

    globe::World* world_;
    Region region_;
    double clock_ = 0.0; // seconds the flow has run
    struct Venting {
        Vent vent;
        double owed = 0.0; // units of lava given and not yet poured
    };
    std::vector<Venting> vents_;
    std::unordered_map<std::uint64_t, double> worn_at_; // clock_ when each brick was last worn
    std::vector<globe::BrickSite> sweeping_; // bricks yet to be looked at this round
    std::unordered_map<std::uint64_t, int> still_; // steps each brick held has gone unchanged
    std::unordered_map<std::uint64_t, Moisture> moisture_;
    std::uint64_t wettings_ = 0; // varies how a fraction of a unit rounds from one to the next
};

} // namespace hog::flow
