#pragma once

#include <hog/client/picture.hpp>
#include <hog/core/result.hpp>

namespace hog::client {

// How far a picture is from another, taking each pixel's largest channel difference and ignoring
// alpha.
struct Difference {
    double mean = 0.0;  // counts
    double share = 0.0; // of pixels differing by more than Likeness::counts
    Picture picture;    // each pixel's difference, as grey brightened to see
};

// How different pictures may be and still be alike, allowing for the drawing changing a little
// between drivers and their versions.
struct Likeness {
    double mean = 1.0; // counts
    int counts = 8;
    double share = 0.01;
};

// Pictures of different sizes fail.
[[nodiscard]] Result<Difference> difference(const Picture& expected, const Picture& drawn,
                                            const Likeness& likeness = {});

[[nodiscard]] bool alike(const Difference& difference, const Likeness& likeness = {}) noexcept;

} // namespace hog::client
