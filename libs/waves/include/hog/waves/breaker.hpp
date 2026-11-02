#pragma once

namespace hog::waves {

// How far a wave over a shoal that's breaking has curled over: the water's extra push along its
// way and up, at a phase (taken within a turn of the crest) of a wave `amplitude` metres high whose
// wavenumber is `wavenumber`. Phase 0 is the crest and positive phases lie ahead of it, on the
// front face; the wave's height is amplitude·cos(phase) before this. At stage 0 there is no curl;
// at 1 the front face has been drawn back under the crest, which hangs out over it as a lip, and
// the lip's tip has dropped.
struct Curl {
    double along = 0.0; // metres ahead
    double up = 0.0;    // metres up
    // Their rates over the phase, per radian, for the surface's slope.
    double along_rate = 0.0;
    double up_rate = 0.0;
};

[[nodiscard]] Curl curl_at(double turned, double stage, double amplitude,
                           double wavenumber) noexcept;

// How far into breaking waves are that would stand `height` metres over their depth's `depth`:
// none until they're as high as the depth holds, all of it once they're half as high again.
[[nodiscard]] double breaking_stage(double height, double depth) noexcept;

// The share of waves that break by plunging, off the Iribarren number of the beach `slope` they
// run up (metres per metre) with offshore `steepness` (height over length): gentle beaches spill
// them into foam, and steep ones let them surge up the face without breaking over.
[[nodiscard]] double plunging(double slope, double offshore_steepness) noexcept;

} // namespace hog::waves
