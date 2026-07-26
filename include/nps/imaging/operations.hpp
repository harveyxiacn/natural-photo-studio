#pragma once

#include "nps/imaging/image16.hpp"

namespace nps::imaging {

// Applies an exposure multiplier of 2^exposure_ev in linear light. Results are
// rounded to the nearest integer sample and clamped to [0, 65535].
[[nodiscard]] Image16 apply_exposure(const Image16& source, double exposure_ev);

} // namespace nps::imaging
