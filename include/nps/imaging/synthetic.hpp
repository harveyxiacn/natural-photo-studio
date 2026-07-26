#pragma once

#include "nps/imaging/image16.hpp"

#include <cstdint>

namespace nps::imaging {

// Generates a deterministic linear RGB gradient. It is suitable for tests and
// demos and contains no imported or personal image data.
[[nodiscard]] Image16 make_deterministic_gradient(
    std::uint32_t width,
    std::uint32_t height);

} // namespace nps::imaging
