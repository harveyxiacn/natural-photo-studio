#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>

namespace nps::imaging {
struct Image16;
struct ImageF32;
} // namespace nps::imaging

namespace nps::color {

class ColorError : public std::invalid_argument {
public:
    using std::invalid_argument::invalid_argument;
};

// Numeric values and stable IDs are persistence/API contracts and must not be
// renumbered or repurposed.
enum class ColorEncoding : std::uint32_t {
    scene_linear_srgb_d65 = 1,
    scene_linear_rec2020_d65 = 2,
};

inline constexpr std::string_view scene_linear_srgb_d65_id =
    "nps.color/scene-linear-srgb-d65/v1";
inline constexpr std::string_view scene_linear_rec2020_d65_id =
    "nps.color/scene-linear-rec2020-d65/v1";

[[nodiscard]] constexpr bool is_supported(
    const ColorEncoding encoding) noexcept {
    return encoding == ColorEncoding::scene_linear_srgb_d65 ||
           encoding == ColorEncoding::scene_linear_rec2020_d65;
}

[[nodiscard]] std::string_view color_encoding_id(ColorEncoding encoding);
[[nodiscard]] std::optional<ColorEncoding> color_encoding_from_id(
    std::string_view stable_id) noexcept;

// A non-empty source rectangle. Region conversion returns an image whose
// dimensions are exactly width x height; output (0, 0) maps to source (x, y).
struct ImageRegion final {
    std::uint32_t x{};
    std::uint32_t y{};
    std::uint32_t width{};
    std::uint32_t height{};
};

// ImageF32 is premultiplied RGBA while Image16 is opaque RGB. Export therefore
// requires an explicit matte, expressed as straight scene-linear RGB in the
// destination encoding. Matte values may be extended-range but must be finite.
struct OpaqueImage16Options final {
    ColorEncoding destination_encoding;
    std::array<float, 3> matte_rgb;
};

// Streaming row primitives. The source and destination spans must represent
// the same non-zero pixel count and be tightly interleaved (RGB or RGBA).
void convert_image16_row_to_opaque_f32(
    std::span<const std::uint16_t> source_rgb,
    ColorEncoding source_encoding,
    ColorEncoding destination_encoding,
    std::span<float> destination_premultiplied_rgba);

void convert_premultiplied_f32_row_to_image16(
    std::span<const float> source_premultiplied_rgba,
    ColorEncoding source_encoding,
    const OpaqueImage16Options& options,
    std::span<std::uint16_t> destination_rgb);

// Direct region conversion. Implementations process one row at a time and do
// not allocate a full-size intermediate image.
[[nodiscard]] imaging::ImageF32 image16_region_to_opaque_f32(
    const imaging::Image16& source,
    ColorEncoding source_encoding,
    ColorEncoding destination_encoding,
    ImageRegion source_region);

[[nodiscard]] imaging::ImageF32 image16_to_opaque_f32(
    const imaging::Image16& source,
    ColorEncoding source_encoding,
    ColorEncoding destination_encoding);

[[nodiscard]] imaging::Image16 premultiplied_f32_region_to_image16(
    const imaging::ImageF32& source,
    const OpaqueImage16Options& options,
    ImageRegion source_region);

[[nodiscard]] imaging::Image16 premultiplied_f32_to_image16(
    const imaging::ImageF32& source,
    const OpaqueImage16Options& options);

} // namespace nps::color
