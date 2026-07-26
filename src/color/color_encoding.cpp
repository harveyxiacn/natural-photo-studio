#include "nps/color/color_encoding.hpp"

#include "nps/imaging/image16.hpp"
#include "nps/imaging/image_f32.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <string>

namespace nps::color {
namespace {

using Matrix3x3 = std::array<double, 9>;

constexpr Matrix3x3 identity_matrix{
    1.0, 0.0, 0.0,
    0.0, 1.0, 0.0,
    0.0, 0.0, 1.0};

// Linear-light D65 matrices derived from the published sRGB and BT.2020
// chromaticities. Coefficients are fixed here as part of the v1 behavior.
constexpr Matrix3x3 linear_srgb_to_rec2020{
    0.627403895934699, 0.329283038377883, 0.043313065687418,
    0.069097289358232, 0.919540395075459, 0.011362315566309,
    0.016391438875150, 0.088013307877226, 0.895595253247624};

constexpr Matrix3x3 linear_rec2020_to_srgb{
    1.660491002108434, -0.587641138788550, -0.072849863319884,
    -0.124550474521591, 1.132899897125960, -0.008349422604369,
    -0.018150763354905, -0.100578898008007, 1.118729661362913};

[[nodiscard]] const Matrix3x3& conversion_matrix(
    const ColorEncoding source,
    const ColorEncoding destination) {
    if (!is_supported(source) || !is_supported(destination)) {
        throw ColorError{"unsupported color encoding"};
    }
    if (source == destination) {
        return identity_matrix;
    }
    if (source == ColorEncoding::scene_linear_srgb_d65 &&
        destination == ColorEncoding::scene_linear_rec2020_d65) {
        return linear_srgb_to_rec2020;
    }
    return linear_rec2020_to_srgb;
}

[[nodiscard]] std::array<double, 3> transform_rgb(
    const Matrix3x3& matrix,
    const double red,
    const double green,
    const double blue) {
    return {
        matrix[0] * red + matrix[1] * green + matrix[2] * blue,
        matrix[3] * red + matrix[4] * green + matrix[5] * blue,
        matrix[6] * red + matrix[7] * green + matrix[8] * blue,
    };
}

void validate_premultiplied_pixel(
    const float red,
    const float green,
    const float blue,
    const float alpha) {
    if (!std::isfinite(red) || !std::isfinite(green) ||
        !std::isfinite(blue) || !std::isfinite(alpha)) {
        throw ColorError{"premultiplied RGBA samples must all be finite"};
    }
    if (alpha < 0.0F || alpha > 1.0F) {
        throw ColorError{"alpha must be in the closed range [0, 1]"};
    }
    if (alpha == 0.0F &&
        (red != 0.0F || green != 0.0F || blue != 0.0F)) {
        throw ColorError{
            "premultiplied RGB must be zero when alpha is zero"};
    }
}

void validate_matte(const OpaqueImage16Options& options) {
    if (!is_supported(options.destination_encoding)) {
        throw ColorError{"unsupported destination color encoding"};
    }
    if (!std::isfinite(options.matte_rgb[0]) ||
        !std::isfinite(options.matte_rgb[1]) ||
        !std::isfinite(options.matte_rgb[2])) {
        throw ColorError{"matte RGB samples must all be finite"};
    }
}

[[nodiscard]] std::size_t checked_multiply(
    const std::size_t left,
    const std::size_t right) {
    if (right != 0 && left > std::numeric_limits<std::size_t>::max() / right) {
        throw ColorError{"row sample count overflows addressable memory"};
    }
    return left * right;
}

void validate_region(
    const ImageRegion region,
    const std::uint32_t image_width,
    const std::uint32_t image_height) {
    if (region.width == 0 || region.height == 0) {
        throw ColorError{"conversion region must be non-empty"};
    }
    if (region.x >= image_width || region.y >= image_height ||
        region.width > image_width - region.x ||
        region.height > image_height - region.y) {
        throw ColorError{"conversion region is outside the source image"};
    }
}

[[nodiscard]] std::uint16_t quantize_u16(const double sample) {
    if (sample <= 0.0) {
        return 0;
    }
    if (sample >= 1.0) {
        return std::numeric_limits<std::uint16_t>::max();
    }
    constexpr auto maximum =
        static_cast<double>(std::numeric_limits<std::uint16_t>::max());
    return static_cast<std::uint16_t>(
        std::floor(sample * maximum + 0.5));
}

} // namespace

std::string_view color_encoding_id(const ColorEncoding encoding) {
    switch (encoding) {
    case ColorEncoding::scene_linear_srgb_d65:
        return scene_linear_srgb_d65_id;
    case ColorEncoding::scene_linear_rec2020_d65:
        return scene_linear_rec2020_d65_id;
    }
    throw ColorError{"unsupported color encoding"};
}

std::optional<ColorEncoding> color_encoding_from_id(
    const std::string_view stable_id) noexcept {
    if (stable_id == scene_linear_srgb_d65_id) {
        return ColorEncoding::scene_linear_srgb_d65;
    }
    if (stable_id == scene_linear_rec2020_d65_id) {
        return ColorEncoding::scene_linear_rec2020_d65;
    }
    return std::nullopt;
}

void convert_image16_row_to_opaque_f32(
    const std::span<const std::uint16_t> source_rgb,
    const ColorEncoding source_encoding,
    const ColorEncoding destination_encoding,
    const std::span<float> destination_premultiplied_rgba) {
    if (source_rgb.empty() || source_rgb.size() % 3 != 0) {
        throw ColorError{
            "source row must contain a non-zero whole number of RGB pixels"};
    }
    const auto pixel_count = source_rgb.size() / 3;
    if (destination_premultiplied_rgba.size() !=
        checked_multiply(pixel_count, std::size_t{4})) {
        throw ColorError{
            "destination RGBA row does not match the source pixel count"};
    }

    const auto& matrix =
        conversion_matrix(source_encoding, destination_encoding);
    constexpr auto maximum =
        static_cast<double>(std::numeric_limits<std::uint16_t>::max());
    for (std::size_t pixel = 0; pixel < pixel_count; ++pixel) {
        const auto source_offset = pixel * 3;
        const auto transformed = transform_rgb(
            matrix,
            static_cast<double>(source_rgb[source_offset]) / maximum,
            static_cast<double>(source_rgb[source_offset + 1]) / maximum,
            static_cast<double>(source_rgb[source_offset + 2]) / maximum);
        const auto destination_offset = pixel * 4;
        for (std::size_t channel = 0; channel < 3; ++channel) {
            const auto value = static_cast<float>(transformed[channel]);
            if (!std::isfinite(value)) {
                throw ColorError{"color conversion produced a non-finite sample"};
            }
            destination_premultiplied_rgba[destination_offset + channel] =
                value;
        }
        destination_premultiplied_rgba[destination_offset + 3] = 1.0F;
    }
}

void convert_premultiplied_f32_row_to_image16(
    const std::span<const float> source_premultiplied_rgba,
    const ColorEncoding source_encoding,
    const OpaqueImage16Options& options,
    const std::span<std::uint16_t> destination_rgb) {
    if (source_premultiplied_rgba.empty() ||
        source_premultiplied_rgba.size() % 4 != 0) {
        throw ColorError{
            "source row must contain a non-zero whole number of RGBA pixels"};
    }
    const auto pixel_count = source_premultiplied_rgba.size() / 4;
    if (destination_rgb.size() !=
        checked_multiply(pixel_count, std::size_t{3})) {
        throw ColorError{
            "destination RGB row does not match the source pixel count"};
    }

    validate_matte(options);
    const auto& matrix =
        conversion_matrix(source_encoding, options.destination_encoding);
    for (std::size_t pixel = 0; pixel < pixel_count; ++pixel) {
        const auto source_offset = pixel * 4;
        const auto red = source_premultiplied_rgba[source_offset];
        const auto green = source_premultiplied_rgba[source_offset + 1];
        const auto blue = source_premultiplied_rgba[source_offset + 2];
        const auto alpha = source_premultiplied_rgba[source_offset + 3];
        validate_premultiplied_pixel(red, green, blue, alpha);

        // Linear matrices may operate directly on premultiplied RGB. Composite
        // only after conversion so transparent edges cannot acquire a halo.
        const auto converted = transform_rgb(
            matrix,
            static_cast<double>(red),
            static_cast<double>(green),
            static_cast<double>(blue));
        const auto inverse_alpha =
            1.0 - static_cast<double>(alpha);
        const auto destination_offset = pixel * 3;
        for (std::size_t channel = 0; channel < 3; ++channel) {
            const auto opaque = converted[channel] +
                inverse_alpha *
                    static_cast<double>(options.matte_rgb[channel]);
            destination_rgb[destination_offset + channel] =
                quantize_u16(opaque);
        }
    }
}

imaging::ImageF32 image16_region_to_opaque_f32(
    const imaging::Image16& source,
    const ColorEncoding source_encoding,
    const ColorEncoding destination_encoding,
    const ImageRegion source_region) {
    source.validate();
    validate_region(source_region, source.width, source.height);
    static_cast<void>(conversion_matrix(
        source_encoding,
        destination_encoding));

    const auto output_sample_count = checked_multiply(
        checked_multiply(
            static_cast<std::size_t>(source_region.width),
            static_cast<std::size_t>(source_region.height)),
        imaging::ImageF32::channel_count);
    imaging::ImageF32 output{
        source_region.width,
        source_region.height,
        destination_encoding,
        std::vector<float>(output_sample_count)};

    const auto source_row_samples =
        checked_multiply(static_cast<std::size_t>(source.width), std::size_t{3});
    const auto region_row_samples =
        checked_multiply(
            static_cast<std::size_t>(source_region.width),
            std::size_t{3});
    for (std::uint32_t output_y = 0;
         output_y < source_region.height;
         ++output_y) {
        const auto source_y =
            static_cast<std::size_t>(source_region.y + output_y);
        const auto source_offset =
            checked_multiply(source_y, source_row_samples) +
            checked_multiply(
                static_cast<std::size_t>(source_region.x),
                std::size_t{3});
        const auto source_row = std::span<const std::uint16_t>{source.samples}
                                    .subspan(
                                        source_offset,
                                        region_row_samples);
        convert_image16_row_to_opaque_f32(
            source_row,
            source_encoding,
            destination_encoding,
            output.row_span(output_y));
    }
    output.validate();
    return output;
}

imaging::ImageF32 image16_to_opaque_f32(
    const imaging::Image16& source,
    const ColorEncoding source_encoding,
    const ColorEncoding destination_encoding) {
    return image16_region_to_opaque_f32(
        source,
        source_encoding,
        destination_encoding,
        ImageRegion{0, 0, source.width, source.height});
}

imaging::Image16 premultiplied_f32_region_to_image16(
    const imaging::ImageF32& source,
    const OpaqueImage16Options& options,
    const ImageRegion source_region) {
    source.validate();
    validate_region(source_region, source.width, source.height);
    validate_matte(options);

    const auto output_sample_count = checked_multiply(
        checked_multiply(
            static_cast<std::size_t>(source_region.width),
            static_cast<std::size_t>(source_region.height)),
        std::size_t{3});
    imaging::Image16 output{
        source_region.width,
        source_region.height,
        std::vector<std::uint16_t>(output_sample_count)};

    const auto region_row_samples = checked_multiply(
        static_cast<std::size_t>(source_region.width),
        imaging::ImageF32::channel_count);
    for (std::uint32_t output_y = 0;
         output_y < source_region.height;
         ++output_y) {
        const auto source_y = source_region.y + output_y;
        const auto source_row = source.row_span(source_y).subspan(
            checked_multiply(
                static_cast<std::size_t>(source_region.x),
                imaging::ImageF32::channel_count),
            region_row_samples);
        const auto destination_offset = checked_multiply(
            static_cast<std::size_t>(output_y),
            checked_multiply(
                static_cast<std::size_t>(source_region.width),
                std::size_t{3}));
        auto destination_row = std::span<std::uint16_t>{output.samples}.subspan(
            destination_offset,
            static_cast<std::size_t>(source_region.width) * 3);
        convert_premultiplied_f32_row_to_image16(
            source_row,
            source.encoding,
            options,
            destination_row);
    }
    output.validate();
    return output;
}

imaging::Image16 premultiplied_f32_to_image16(
    const imaging::ImageF32& source,
    const OpaqueImage16Options& options) {
    return premultiplied_f32_region_to_image16(
        source,
        options,
        ImageRegion{0, 0, source.width, source.height});
}

} // namespace nps::color
