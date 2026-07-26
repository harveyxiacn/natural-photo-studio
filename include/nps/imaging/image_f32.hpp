#pragma once

#include "nps/color/color_encoding.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace nps::imaging {

class ImageF32Error : public std::invalid_argument {
public:
    using std::invalid_argument::invalid_argument;
};

// Scene-linear, interleaved, premultiplied RGBA.
//
// RGB is intentionally unbounded for non-zero alpha so that negative and HDR
// scene values survive intermediate operations. Alpha is always in [0, 1].
struct ImageF32 final {
    static constexpr std::size_t channel_count = 4;

    std::uint32_t width{};
    std::uint32_t height{};
    color::ColorEncoding encoding{
        color::ColorEncoding::scene_linear_rec2020_d65};
    std::vector<float> samples{};

    [[nodiscard]] std::size_t expected_sample_count() const;
    void validate() const;

    // These checked views never allocate. They verify dimensions, storage
    // length, and coordinates before exposing the requested memory.
    [[nodiscard]] std::span<float> row_span(std::uint32_t y);
    [[nodiscard]] std::span<const float> row_span(std::uint32_t y) const;
    [[nodiscard]] std::span<float, channel_count> pixel_span(
        std::uint32_t x,
        std::uint32_t y);
    [[nodiscard]] std::span<const float, channel_count> pixel_span(
        std::uint32_t x,
        std::uint32_t y) const;

    friend bool operator==(const ImageF32&, const ImageF32&) = default;
};

// Returns the lowercase SHA-256 of the validated canonical pixel stream.
//
// The stream starts with the ASCII bytes
// "nps.image-f32/canonical-sha256/v1" (without a terminator), then contains
// big-endian uint32 width and height, a big-endian uint32 byte length and the
// stable color-encoding ID bytes, and row-major big-endian IEEE-754 binary32
// RGBA samples. Signed zero is normalized to positive zero.
[[nodiscard]] std::string image_f32_sha256(const ImageF32& image);

} // namespace nps::imaging
