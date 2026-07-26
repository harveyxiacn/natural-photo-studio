#include "nps/color/color_encoding.hpp"
#include "nps/imaging/image16.hpp"
#include "nps/imaging/image_f32.hpp"
#include "nps/imaging/mask16.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace {

using nps::color::ColorEncoding;

constexpr auto linear_srgb = ColorEncoding::scene_linear_srgb_d65;
constexpr auto linear_rec2020 =
    ColorEncoding::scene_linear_rec2020_d65;

} // namespace

TEST_CASE("color encodings have stable persisted identities") {
    REQUIRE(static_cast<std::uint32_t>(linear_srgb) == 1);
    REQUIRE(static_cast<std::uint32_t>(linear_rec2020) == 2);
    REQUIRE(
        nps::color::color_encoding_id(linear_srgb) ==
        "nps.color/scene-linear-srgb-d65/v1");
    REQUIRE(
        nps::color::color_encoding_id(linear_rec2020) ==
        "nps.color/scene-linear-rec2020-d65/v1");
    REQUIRE(
        nps::color::color_encoding_from_id(
            "nps.color/scene-linear-rec2020-d65/v1") ==
        linear_rec2020);
    REQUIRE_FALSE(nps::color::color_encoding_from_id("unknown"));
    REQUIRE_THROWS_AS(
        nps::color::color_encoding_id(
            static_cast<ColorEncoding>(999)),
        nps::color::ColorError);
}

TEST_CASE("ImageF32 enforces premultiplied finite RGBA without clipping RGB") {
    nps::imaging::ImageF32 image{
        3,
        1,
        linear_rec2020,
        {
            0.0F, -0.0F, 0.0F, 0.0F,
            -0.25F, 2.0F, 0.5F, 0.25F,
            4.0F, -3.0F, 1.5F, 1.0F,
        }};
    REQUIRE_NOTHROW(image.validate());
    REQUIRE(image.pixel_span(1, 0)[0] == -0.25F);
    REQUIRE(image.pixel_span(1, 0)[1] == 2.0F);

    image.pixel_span(0, 0)[0] = 0.001F;
    REQUIRE_THROWS_AS(image.validate(), nps::imaging::ImageF32Error);
    image.pixel_span(0, 0)[0] = 0.0F;

    image.pixel_span(1, 0)[3] = -0.01F;
    REQUIRE_THROWS_AS(image.validate(), nps::imaging::ImageF32Error);
    image.pixel_span(1, 0)[3] = 0.25F;

    image.pixel_span(1, 0)[2] = std::numeric_limits<float>::quiet_NaN();
    REQUIRE_THROWS_AS(image.validate(), nps::imaging::ImageF32Error);
    image.pixel_span(1, 0)[2] = std::numeric_limits<float>::infinity();
    REQUIRE_THROWS_AS(image.validate(), nps::imaging::ImageF32Error);
}

TEST_CASE("ImageF32 checked views reject malformed storage and coordinates") {
    nps::imaging::ImageF32 image{
        2,
        2,
        linear_srgb,
        std::vector<float>(16, 0.0F)};
    REQUIRE(image.row_span(1).size() == 8);
    image.pixel_span(1, 1)[3] = 1.0F;
    REQUIRE(image.samples[15] == 1.0F);
    REQUIRE_THROWS_AS(image.row_span(2), nps::imaging::ImageF32Error);
    REQUIRE_THROWS_AS(image.pixel_span(2, 0), nps::imaging::ImageF32Error);

    image.samples.pop_back();
    REQUIRE_THROWS_AS(image.row_span(0), nps::imaging::ImageF32Error);

    const nps::imaging::ImageF32 overflowing{
        std::numeric_limits<std::uint32_t>::max(),
        std::numeric_limits<std::uint32_t>::max(),
        linear_srgb,
        {}};
    REQUIRE_THROWS_AS(
        overflowing.expected_sample_count(),
        nps::imaging::ImageF32Error);
}

TEST_CASE("ImageF32 canonical hashes are stable and content-sensitive") {
    const nps::imaging::ImageF32 image{
        2,
        1,
        linear_rec2020,
        {
            0.25F, -0.5F, 2.0F, 0.5F,
            0.0F, 0.0F, 0.0F, 0.0F,
        }};
    const auto hash = nps::imaging::image_f32_sha256(image);
    REQUIRE(
        hash ==
        "bfc027dbea0d0e0d35972442c8418a8d62213f1881fa03a64b243a3ae0e68348");
    REQUIRE(nps::imaging::image_f32_sha256(image) == hash);
    REQUIRE(nps::imaging::image_f32_sha256(
                nps::imaging::ImageF32{image}) == hash);

    auto changed_sample = image;
    changed_sample.samples[0] = 0.2501F;
    REQUIRE(nps::imaging::image_f32_sha256(changed_sample) != hash);

    auto changed_dimensions = image;
    changed_dimensions.width = 1;
    changed_dimensions.height = 2;
    REQUIRE(nps::imaging::image_f32_sha256(changed_dimensions) != hash);

    auto changed_encoding = image;
    changed_encoding.encoding = linear_srgb;
    REQUIRE(nps::imaging::image_f32_sha256(changed_encoding) != hash);

    const nps::imaging::ImageF32 positive_zero{
        1,
        1,
        linear_rec2020,
        {0.0F, 0.0F, 0.0F, 0.0F}};
    const nps::imaging::ImageF32 negative_zero{
        1,
        1,
        linear_rec2020,
        {-0.0F, 0.0F, -0.0F, -0.0F}};
    REQUIRE(
        nps::imaging::image_f32_sha256(negative_zero) ==
        nps::imaging::image_f32_sha256(positive_zero));

    auto malformed = image;
    malformed.samples.pop_back();
    REQUIRE_THROWS_AS(
        nps::imaging::image_f32_sha256(malformed),
        nps::imaging::ImageF32Error);

    auto non_finite = image;
    non_finite.samples[0] = std::numeric_limits<float>::quiet_NaN();
    REQUIRE_THROWS_AS(
        nps::imaging::image_f32_sha256(non_finite),
        nps::imaging::ImageF32Error);
}

TEST_CASE("Mask16 sampling inversion and strength are deterministic") {
    const nps::imaging::Mask16 mask{
        3,
        1,
        {0, 32767, 65535}};
    REQUIRE_NOTHROW(mask.validate());
    REQUIRE(mask.sample(1, 0) == 32767);
    REQUIRE(mask.coverage(2, 0) == 1.0F);
    REQUIRE(
        mask.inverted().samples ==
        std::vector<std::uint16_t>{65535, 32768, 0});
    REQUIRE(
        mask.with_strength(0.5F).samples ==
        std::vector<std::uint16_t>{0, 16384, 32768});
    REQUIRE(
        mask.with_strength(0.0F).samples ==
        std::vector<std::uint16_t>{0, 0, 0});

    REQUIRE_THROWS_AS(mask.sample(3, 0), nps::imaging::MaskError);
    REQUIRE_THROWS_AS(mask.row_span(1), nps::imaging::MaskError);
    REQUIRE_THROWS_AS(mask.with_strength(-0.01F), nps::imaging::MaskError);
    REQUIRE_THROWS_AS(
        mask.with_strength(std::numeric_limits<float>::infinity()),
        nps::imaging::MaskError);

    const nps::imaging::Mask16 malformed{2, 2, {0, 1, 2}};
    REQUIRE_THROWS_AS(malformed.validate(), nps::imaging::MaskError);
    REQUIRE_THROWS_AS(malformed.row_span(0), nps::imaging::MaskError);
}

TEST_CASE("Mask16 canonical hashes are stable and content-sensitive") {
    const nps::imaging::Mask16 mask{
        2,
        2,
        {0, 1, 32768, 65535}};
    const auto hash = nps::imaging::mask16_sha256(mask);
    REQUIRE(
        hash ==
        "248ddef27a3f0045526f015d4de4f003618df6ba87df77ad4601304579b088ca");
    REQUIRE(nps::imaging::mask16_sha256(mask) == hash);
    REQUIRE(nps::imaging::mask16_sha256(
                nps::imaging::Mask16{mask}) == hash);

    auto changed_sample = mask;
    changed_sample.samples[1] = 2;
    REQUIRE(nps::imaging::mask16_sha256(changed_sample) != hash);

    auto changed_dimensions = mask;
    changed_dimensions.width = 1;
    changed_dimensions.height = 4;
    REQUIRE(nps::imaging::mask16_sha256(changed_dimensions) != hash);

    auto malformed = mask;
    malformed.samples.pop_back();
    REQUIRE_THROWS_AS(
        nps::imaging::mask16_sha256(malformed),
        nps::imaging::MaskError);
}

TEST_CASE("linear sRGB to Rec2020 conversion matches fixed primary baseline") {
    const nps::imaging::Image16 red{1, 1, {65535, 0, 0}};
    const auto converted = nps::color::image16_to_opaque_f32(
        red,
        linear_srgb,
        linear_rec2020);

    const auto pixel = converted.pixel_span(0, 0);
    REQUIRE(pixel[0] == Catch::Approx(0.627403896F).margin(0.0000001F));
    REQUIRE(pixel[1] == Catch::Approx(0.069097288F).margin(0.0000001F));
    REQUIRE(pixel[2] == Catch::Approx(0.016391439F).margin(0.0000001F));
    REQUIRE(pixel[3] == 1.0F);
}

TEST_CASE("wide-gamut conversion preserves negative and over-range values") {
    const std::vector<float> rec2020_red{1.0F, 0.0F, 0.0F, 1.0F};
    std::vector<std::uint16_t> unused_rgb(3);
    const nps::color::OpaqueImage16Options black_srgb{
        linear_srgb,
        {0.0F, 0.0F, 0.0F}};

    // The row exporter clips only at its final integer boundary.
    nps::color::convert_premultiplied_f32_row_to_image16(
        rec2020_red,
        linear_rec2020,
        black_srgb,
        unused_rgb);
    REQUIRE(
        unused_rgb ==
        std::vector<std::uint16_t>{65535, 0, 0});

    const nps::imaging::Image16 source{1, 1, {65535, 0, 0}};
    const auto extended = nps::color::image16_to_opaque_f32(
        source,
        linear_rec2020,
        linear_srgb);
    REQUIRE(extended.samples[0] > 1.0F);
    REQUIRE(extended.samples[1] < 0.0F);
    REQUIRE(extended.samples[2] < 0.0F);
    REQUIRE_NOTHROW(extended.validate());
}

TEST_CASE("opaque export composites premultiplied transparent edges") {
    const nps::imaging::ImageF32 edge{
        2,
        1,
        linear_srgb,
        {
            0.25F, 0.0F, 0.0F, 0.25F,
            0.0F, 0.0F, 0.0F, 0.0F,
        }};
    const nps::color::OpaqueImage16Options white{
        linear_srgb,
        {1.0F, 1.0F, 1.0F}};

    const auto flattened =
        nps::color::premultiplied_f32_to_image16(edge, white);
    REQUIRE(
        flattened.samples ==
        std::vector<std::uint16_t>{
            65535, 49151, 49151,
            65535, 65535, 65535});
}

TEST_CASE("final Image16 quantization is clamped round-half-up") {
    const nps::imaging::ImageF32 source{
        2,
        1,
        linear_srgb,
        {
            0.5F, -2.0F, 3.0F, 1.0F,
            0.0F, 0.0F, 0.0F, 0.0F,
        }};
    const nps::color::OpaqueImage16Options black{
        linear_srgb,
        {0.0F, 0.0F, 0.0F}};

    const auto encoded =
        nps::color::premultiplied_f32_to_image16(source, black);
    REQUIRE(
        encoded.samples ==
        std::vector<std::uint16_t>{32768, 0, 65535, 0, 0, 0});
}

TEST_CASE("region and row conversions have exact coordinates and sizes") {
    const nps::imaging::Image16 source{
        3,
        2,
        {
            1, 2, 3, 4, 5, 6, 7, 8, 9,
            10, 11, 12, 13, 14, 15, 16, 17, 18,
        }};
    const auto region = nps::color::image16_region_to_opaque_f32(
        source,
        linear_srgb,
        linear_srgb,
        nps::color::ImageRegion{1, 0, 2, 2});
    REQUIRE(region.width == 2);
    REQUIRE(region.height == 2);
    REQUIRE(
        region.pixel_span(0, 0)[0] ==
        Catch::Approx(4.0F / 65535.0F));
    REQUIRE(
        region.pixel_span(1, 1)[2] ==
        Catch::Approx(18.0F / 65535.0F));

    const nps::color::OpaqueImage16Options black{
        linear_srgb,
        {0.0F, 0.0F, 0.0F}};
    const auto round_trip = nps::color::premultiplied_f32_region_to_image16(
        region,
        black,
        nps::color::ImageRegion{0, 1, 2, 1});
    REQUIRE(round_trip.width == 2);
    REQUIRE(round_trip.height == 1);
    REQUIRE(
        round_trip.samples ==
        std::vector<std::uint16_t>{13, 14, 15, 16, 17, 18});

    REQUIRE_THROWS_AS(
        nps::color::image16_region_to_opaque_f32(
            source,
            linear_srgb,
            linear_srgb,
            nps::color::ImageRegion{2, 0, 2, 1}),
        nps::color::ColorError);

    std::vector<float> wrong_row(7);
    REQUIRE_THROWS_AS(
        nps::color::convert_image16_row_to_opaque_f32(
            std::span<const std::uint16_t>{source.samples}.first(6),
            linear_srgb,
            linear_srgb,
            wrong_row),
        nps::color::ColorError);
}

TEST_CASE("conversion rejects NaN Inf and invalid transparent RGB") {
    const nps::color::OpaqueImage16Options black{
        linear_srgb,
        {0.0F, 0.0F, 0.0F}};
    std::vector<std::uint16_t> output(3);

    const std::vector<float> nan_pixel{
        std::numeric_limits<float>::quiet_NaN(), 0.0F, 0.0F, 1.0F};
    REQUIRE_THROWS_AS(
        nps::color::convert_premultiplied_f32_row_to_image16(
            nan_pixel,
            linear_srgb,
            black,
            output),
        nps::color::ColorError);

    const std::vector<float> invalid_transparent{
        0.1F, 0.0F, 0.0F, 0.0F};
    REQUIRE_THROWS_AS(
        nps::color::convert_premultiplied_f32_row_to_image16(
            invalid_transparent,
            linear_srgb,
            black,
            output),
        nps::color::ColorError);

    const nps::color::OpaqueImage16Options infinite_matte{
        linear_srgb,
        {0.0F, std::numeric_limits<float>::infinity(), 0.0F}};
    const std::vector<float> opaque{0.0F, 0.0F, 0.0F, 1.0F};
    REQUIRE_THROWS_AS(
        nps::color::convert_premultiplied_f32_row_to_image16(
            opaque,
            linear_srgb,
            infinite_matte,
            output),
        nps::color::ColorError);
}
