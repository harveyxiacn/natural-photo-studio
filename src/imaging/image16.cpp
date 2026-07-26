#include "nps/imaging/image16.hpp"

#include <limits>
#include <string>

namespace nps::imaging {
namespace {

[[nodiscard]] std::size_t checked_multiply(
    const std::size_t left,
    const std::size_t right) {
    if (right != 0 && left > std::numeric_limits<std::size_t>::max() / right) {
        throw ImageError{"image dimensions overflow the addressable sample count"};
    }
    return left * right;
}

} // namespace

std::size_t Image16::expected_sample_count() const {
    if (width == 0 || height == 0) {
        throw ImageError{"image width and height must both be greater than zero"};
    }

    const auto pixels = checked_multiply(
        static_cast<std::size_t>(width),
        static_cast<std::size_t>(height));
    return checked_multiply(pixels, channel_count);
}

void Image16::validate() const {
    const auto expected = expected_sample_count();
    if (samples.size() != expected) {
        throw ImageError{
            "image has " + std::to_string(samples.size()) +
            " samples but its dimensions require " + std::to_string(expected)};
    }
}

} // namespace nps::imaging
