#include "nps/imaging/synthetic.hpp"

#include <cstddef>
#include <cstdint>

namespace nps::imaging {
namespace {

[[nodiscard]] std::uint16_t gradient_sample(
    const std::uint32_t index,
    const std::uint32_t extent) {
    if (extent == 1) {
        return 0;
    }

    constexpr std::uint64_t maximum = 65535;
    const auto denominator = static_cast<std::uint64_t>(extent - 1);
    const auto numerator =
        static_cast<std::uint64_t>(index) * maximum + denominator / 2;
    return static_cast<std::uint16_t>(numerator / denominator);
}

} // namespace

Image16 make_deterministic_gradient(
    const std::uint32_t width,
    const std::uint32_t height) {
    Image16 image{width, height, {}};
    image.samples.resize(image.expected_sample_count());

    for (std::uint32_t y = 0; y < height; ++y) {
        const auto green = gradient_sample(y, height);
        for (std::uint32_t x = 0; x < width; ++x) {
            const auto red = gradient_sample(x, width);
            const auto blue = static_cast<std::uint16_t>(
                (static_cast<std::uint32_t>(red) + green) / 2U);
            const auto sample_index =
                (static_cast<std::size_t>(y) * width + x) *
                Image16::channel_count;
            image.samples[sample_index] = red;
            image.samples[sample_index + 1] = green;
            image.samples[sample_index + 2] = blue;
        }
    }

    return image;
}

} // namespace nps::imaging
