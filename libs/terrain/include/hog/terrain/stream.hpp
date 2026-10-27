#pragma once

#include <hog/core/flat_map.hpp>
#include <hog/core/math.hpp>
#include <hog/planet/cell_id.hpp>
#include <hog/terrain/patch.hpp>
#include <hog/terrain/selection.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <vector>

namespace hog::terrain {

struct StreamSettings {
    std::uint32_t capacity = 4'096; // patches held at once
    std::uint32_t uploads = 16;     // patches arriving in a frame at most
    std::uint32_t bakes = 16;       // in flight at most
    // Levels below the nearest ground of known heights that a bake is asked for at most, so
    // coarser ground is baked first, bounding the finer ground's heights and drawn in its place
    // meanwhile. 0 bakes the selection straight away.
    int coarse_steps = 3;
    // How far ground of unknown heights may lie beyond its nearest known ancestor's, in that
    // ancestor's vertex spacings.
    double margin = 2.0;
};

// A held patch, drawn in place of the selection over its ground.
struct StreamedPatch {
    planet::CellId cell;
    // NOLINTNEXTLINE(readability-redundant-member-init): lets designated initializers omit it
    WorldPosition origin = {};
    std::uint8_t coarser_edges = 0; // edge bits, where the patch beside it is coarser

    friend bool operator==(const StreamedPatch&, const StreamedPatch&) = default;
};

struct StreamFrame {
    std::vector<planet::CellId> evicted; // no longer held
    std::vector<Patch> arrived;          // held from now on, to upload, replacing any held before
    std::vector<StreamedPatch> drawn;    // nearest first
    std::vector<planet::CellId> bakes;   // to start, most wanted first
};

// Which patches are held, standing in for the selection until what it asks for is baked: a held
// ancestor covers its descendants until all of them are held, and held children cover their
// parent. An ancestor only starts standing in where nothing finer is drawn beneath it, so ground
// never turns coarse again; what it would have covered is left out until baked. Ground covered by
// nothing held is baked first, then the rest nearest first, each from coarse to fine. The patches
// drawn least recently make room for those arriving. Changed ground stays drawn until it's baked
// again.
class PatchStream {
public:
    explicit PatchStream(const StreamSettings& settings = {}) : settings_(settings) {}

    // A patch an update asked to bake, baked.
    void baked(Patch patch);
    // A cell of the world changed, or had a change rolled back. No bake may be running while the
    // world changes; those in flight across it are dropped when they arrive.
    void changed(planet::CellId cell);
    // Once a frame, with the patches selected for it. Those in view and not under the sea are
    // drawn; all are baked and kept, so selecting all round the eye has ground ready before it
    // turns into view.
    [[nodiscard]] StreamFrame update(std::span<const SelectedPatch> selected);
    // What would be drawn of the same selection all round the eye, in view or not, as held now:
    // the ground out of the sea as a view in every direction at once sees it.
    [[nodiscard]] std::vector<StreamedPatch>
    drawn_all_round(std::span<const SelectedPatch> selected) const;
    // Whether the last update left nothing to do: none baking or waiting, and none asked for,
    // arriving or evicted. Settled, the same view selects the same ground, and updating with it
    // draws the same again, so it's answered at once.
    [[nodiscard]] bool settled() const noexcept { return settled_; }

    [[nodiscard]] bool held(planet::CellId cell) const { return held_.contains(cell); }
    [[nodiscard]] std::size_t held_count() const noexcept { return held_.size(); }
    [[nodiscard]] std::size_t baking_count() const noexcept { return baking_.size(); }

    // Of a patch baked so far, or bounding one whose children have all been.
    [[nodiscard]] std::optional<HeightRange> heights(planet::CellId cell) const;
    // heights(), or else its nearest ancestor's with the margin, as far as anything's known; with
    // how far the ground strays from its patch, or its ancestor's halved for each level below it.
    [[nodiscard]] std::optional<GroundBounds> bounds(planet::CellId cell) const;
    // bounds(), for selection, for as long as the stream lives.
    [[nodiscard]] KnownGround known_ground() const {
        return [this](planet::CellId cell) { return bounds(cell); };
    }

private:
    struct Held {
        WorldPosition origin;
        std::uint64_t drawn = 0; // frame
        bool stale = false;      // its ground changed since
    };
    struct Known {
        HeightRange heights;
        double spacing = 0.0; // metres between the vertices the heights were taken from
        bool baked = false;   // rather than bounding its children
        // Metres the ground strays from its patch: as its children measured, once all of them
        // have, or else as from a level coarser, which overstates it.
        double error = 0.0;
        // Metres the ground strays from its parent's patch, as a bake measures it; twice its own
        // otherwise.
        double coarser = 0.0;
    };

    // Held cells covering the selection, without overlapping, with the least distance of what
    // each covers.
    [[nodiscard]] std::map<planet::CellId, double>
    cover(std::span<const SelectedPatch> selected) const;
    // The held cells covering `chosen`, stitched to one another, nearest first.
    [[nodiscard]] std::vector<StreamedPatch> drawn_of(std::span<const SelectedPatch> chosen) const;
    // Adds to `drawn` the ancestors that may stand in for missing ground, with the least
    // distance of what each stands in for.
    void stand_in(const std::map<planet::CellId, double>& standing,
                  std::map<planet::CellId, double>& drawn) const;
    [[nodiscard]] bool covered(planet::CellId cell) const;
    // Held, and baked since its ground last changed.
    [[nodiscard]] bool fresh(planet::CellId cell) const;
    [[nodiscard]] std::optional<planet::CellId> held_above(planet::CellId cell) const;
    // The coarsest ancestor still to bake before a selected cell, or the cell itself.
    [[nodiscard]] planet::CellId to_bake(planet::CellId cell) const;
    // The waiting patches still wanted: selected, or standing in for selected ground within them,
    // as near as that ground is.
    [[nodiscard]] std::map<planet::CellId, double>
    wanted_waiting(std::span<const SelectedPatch> selected) const;
    void arrive(std::span<const SelectedPatch> selected, StreamFrame& frame);
    void spread_heights(planet::CellId cell);
    // The most the ground strays from the patch over all four children, if they're all known.
    [[nodiscard]] std::optional<Known> from_children(planet::CellId cell) const;

    StreamSettings settings_;
    // Looked up for every patch selected, every frame, but changed by a few at most.
    FlatMap<planet::CellId, Held> held_;
    std::set<planet::CellId> baking_;
    std::set<planet::CellId> outdated_; // baking from ground since changed
    std::vector<Patch> waiting_;        // baked, not yet arrived
    std::map<planet::CellId, Known> heights_;
    std::uint64_t frame_ = 0;
    bool settled_ = false;
    std::vector<SelectedPatch> settled_on_;
    std::vector<StreamedPatch> settled_drawn_;
};

} // namespace hog::terrain
