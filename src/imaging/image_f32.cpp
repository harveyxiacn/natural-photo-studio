#include "nps/imaging/image_f32.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

#include <openssl/evp.h>

namespace nps::imaging {
namespace {

constexpr std::string_view kImageF32HashDomain =
    "nps.image-f32/canonical-sha256/v1";
constexpr std::size_t kSha256Bytes = 32U;
constexpr std::size_t kHashBufferBytes = 16U * 1024U;

static_assert(sizeof(float) == sizeof(std::uint32_t));
static_assert(std::numeric_limits<float>::is_iec559);

class Sha256Context final {
public:
    Sha256Context()
        : context_(EVP_MD_CTX_new(), &EVP_MD_CTX_free) {
        if (context_ == nullptr ||
            EVP_DigestInit_ex(context_.get(), EVP_sha256(), nullptr) != 1) {
            throw std::runtime_error{
                "Unable to calculate the canonical image hash."};
        }
    }

    void update(const void* data, const std::size_t size) {
        if (EVP_DigestUpdate(context_.get(), data, size) != 1) {
            throw std::runtime_error{
                "Unable to calculate the canonical image hash."};
        }
    }

    [[nodiscard]] std::string finish() {
        std::array<unsigned char, kSha256Bytes> digest{};
        unsigned int digest_size = 0U;
        if (EVP_DigestFinal_ex(
                context_.get(),
                digest.data(),
                &digest_size) != 1 ||
            digest_size != kSha256Bytes) {
            throw std::runtime_error{
                "Unable to calculate the canonical image hash."};
        }

        constexpr std::array<char, 16> hexadecimal{
            '0', '1', '2', '3', '4', '5', '6', '7',
            '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
        std::string result;
        result.reserve(kSha256Bytes * 2U);
        for (const unsigned char value : digest) {
            result.push_back(
                hexadecimal[static_cast<std::size_t>(value >> 4U)]);
            result.push_back(
                hexadecimal[static_cast<std::size_t>(value & 0x0FU)]);
        }
        return result;
    }

private:
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context_;
};

[[nodiscard]] constexpr std::array<unsigned char, 4> big_endian_uint32(
    const std::uint32_t value) noexcept {
    return {
        static_cast<unsigned char>((value >> 24U) & 0xFFU),
        static_cast<unsigned char>((value >> 16U) & 0xFFU),
        static_cast<unsigned char>((value >> 8U) & 0xFFU),
        static_cast<unsigned char>(value & 0xFFU),
    };
}

[[nodiscard]] std::size_t checked_multiply(
    const std::size_t left,
    const std::size_t right) {
    if (right != 0 && left > std::numeric_limits<std::size_t>::max() / right) {
        throw ImageF32Error{
            "image dimensions overflow the addressable sample count"};
    }
    return left * right;
}

[[nodiscard]] std::size_t checked_row_offset(
    const ImageF32& image,
    const std::uint32_t y) {
    const auto expected = image.expected_sample_count();
    if (image.samples.size() != expected) {
        throw ImageF32Error{
            "image has " + std::to_string(image.samples.size()) +
            " samples but its dimensions require " + std::to_string(expected)};
    }
    if (y >= image.height) {
        throw ImageF32Error{"row coordinate is outside the image"};
    }
    const auto row_samples = checked_multiply(
        static_cast<std::size_t>(image.width),
        ImageF32::channel_count);
    return checked_multiply(static_cast<std::size_t>(y), row_samples);
}

} // namespace

std::size_t ImageF32::expected_sample_count() const {
    if (width == 0 || height == 0) {
        throw ImageF32Error{
            "image width and height must both be greater than zero"};
    }

    const auto pixels = checked_multiply(
        static_cast<std::size_t>(width),
        static_cast<std::size_t>(height));
    return checked_multiply(pixels, channel_count);
}

void ImageF32::validate() const {
    if (!color::is_supported(encoding)) {
        throw ImageF32Error{"image has an unsupported color encoding"};
    }

    const auto expected = expected_sample_count();
    if (samples.size() != expected) {
        throw ImageF32Error{
            "image has " + std::to_string(samples.size()) +
            " samples but its dimensions require " + std::to_string(expected)};
    }

    for (std::size_t offset = 0; offset < samples.size();
         offset += channel_count) {
        const auto red = samples[offset];
        const auto green = samples[offset + 1];
        const auto blue = samples[offset + 2];
        const auto alpha = samples[offset + 3];
        if (!std::isfinite(red) || !std::isfinite(green) ||
            !std::isfinite(blue) || !std::isfinite(alpha)) {
            throw ImageF32Error{"image samples must all be finite"};
        }
        if (alpha < 0.0F || alpha > 1.0F) {
            throw ImageF32Error{"image alpha must be in the closed range [0, 1]"};
        }
        if (alpha == 0.0F &&
            (red != 0.0F || green != 0.0F || blue != 0.0F)) {
            throw ImageF32Error{
                "premultiplied RGB must be zero when alpha is zero"};
        }
    }
}

std::span<float> ImageF32::row_span(const std::uint32_t y) {
    const auto offset = checked_row_offset(*this, y);
    const auto row_samples =
        static_cast<std::size_t>(width) * channel_count;
    return std::span<float>{samples}.subspan(offset, row_samples);
}

std::span<const float> ImageF32::row_span(const std::uint32_t y) const {
    const auto offset = checked_row_offset(*this, y);
    const auto row_samples =
        static_cast<std::size_t>(width) * channel_count;
    return std::span<const float>{samples}.subspan(offset, row_samples);
}

std::span<float, ImageF32::channel_count> ImageF32::pixel_span(
    const std::uint32_t x,
    const std::uint32_t y) {
    if (x >= width) {
        throw ImageF32Error{"column coordinate is outside the image"};
    }
    auto row = row_span(y);
    const auto offset = static_cast<std::size_t>(x) * channel_count;
    return std::span<float, channel_count>{
        row.data() + offset,
        channel_count};
}

std::span<const float, ImageF32::channel_count> ImageF32::pixel_span(
    const std::uint32_t x,
    const std::uint32_t y) const {
    if (x >= width) {
        throw ImageF32Error{"column coordinate is outside the image"};
    }
    const auto row = row_span(y);
    const auto offset = static_cast<std::size_t>(x) * channel_count;
    return std::span<const float, channel_count>{
        row.data() + offset,
        channel_count};
}

std::string image_f32_sha256(const ImageF32& image) {
    image.validate();

    const std::string_view encoding_id =
        color::color_encoding_id(image.encoding);
    Sha256Context hash;
    hash.update(kImageF32HashDomain.data(), kImageF32HashDomain.size());

    const auto width = big_endian_uint32(image.width);
    const auto height = big_endian_uint32(image.height);
    const auto encoding_size = big_endian_uint32(
        static_cast<std::uint32_t>(encoding_id.size()));
    hash.update(width.data(), width.size());
    hash.update(height.data(), height.size());
    hash.update(encoding_size.data(), encoding_size.size());
    hash.update(encoding_id.data(), encoding_id.size());

    std::array<unsigned char, kHashBufferBytes> buffer{};
    std::size_t used = 0U;
    for (const float sample : image.samples) {
        const std::uint32_t bits = sample == 0.0F
            ? 0U
            : std::bit_cast<std::uint32_t>(sample);
        const auto encoded = big_endian_uint32(bits);
        for (const unsigned char byte : encoded) {
            buffer[used++] = byte;
        }
        if (used == buffer.size()) {
            hash.update(buffer.data(), used);
            used = 0U;
        }
    }
    if (used != 0U) {
        hash.update(buffer.data(), used);
    }
    return hash.finish();
}

} // namespace nps::imaging
