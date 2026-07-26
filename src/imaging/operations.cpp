#include "nps/imaging/operations.hpp"

#include <cmath>
#include <cstdint>
#include <limits>

namespace nps::imaging {

Image16 apply_exposure(const Image16& source, const double exposure_ev) {
    source.validate();
    if (!std::isfinite(exposure_ev)) {
        throw ImageError{"exposure EV must be finite"};
    }

    Image16 result = source;
    const double multiplier = std::exp2(exposure_ev);
    constexpr auto maximum = std::numeric_limits<std::uint16_t>::max();
    constexpr double maximum_as_double = static_cast<double>(maximum);

    for (auto& sample : result.samples) {
        if (sample == 0 || multiplier == 0.0) {
            sample = 0;
            continue;
        }

        if (std::isinf(multiplier)) {
            sample = maximum;
            continue;
        }

        const double scaled = static_cast<double>(sample) * multiplier;
        if (scaled >= maximum_as_double) {
            sample = maximum;
        } else if (scaled <= 0.0) {
            sample = 0;
        } else {
            sample = static_cast<std::uint16_t>(std::floor(scaled + 0.5));
        }
    }

    return result;
}

} // namespace nps::imaging
