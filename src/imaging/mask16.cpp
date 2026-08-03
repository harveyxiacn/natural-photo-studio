#include "nps/imaging/mask16.hpp"

#include <algorithm>
#include <array>
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

constexpr std::string_view kMask16HashDomain =
    "nps.mask16/canonical-sha256/v1";
constexpr std::size_t kSha256Bytes = 32U;
constexpr std::size_t kHashBufferBytes = 16U * 1024U;

class Sha256Context final {
public:
    Sha256Context()
        : context_(EVP_MD_CTX_new(), &EVP_MD_CTX_free) {
        if (context_ == nullptr ||
            EVP_DigestInit_ex(context_.get(), EVP_sha256(), nullptr) != 1) {
            throw std::runtime_error{
                "Unable to calculate the canonical mask hash."};
        }
    }

    void update(const void* data, const std::size_t size) {
        if (EVP_DigestUpdate(context_.get(), data, size) != 1) {
            throw std::runtime_error{
                "Unable to calculate the canonical mask hash."};
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
                "Unable to calculate the canonical mask hash."};
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
        throw MaskError{
            "mask dimensions overflow the addressable sample count"};
    }
    return left * right;
}

[[nodiscard]] std::size_t checked_row_offset(
    const Mask16& mask,
    const std::uint32_t y) {
    const auto expected = mask.expected_sample_count();
    if (mask.samples.size() != expected) {
        throw MaskError{
            "mask has " + std::to_string(mask.samples.size()) +
            " samples but its dimensions require " + std::to_string(expected)};
    }
    if (y >= mask.height) {
        throw MaskError{"row coordinate is outside the mask"};
    }
    return checked_multiply(
        static_cast<std::size_t>(y),
        static_cast<std::size_t>(mask.width));
}

[[nodiscard]] std::uint16_t quantize_mask_sample(const double value) {
    const auto rounded = std::floor(value + 0.5);
    const auto bounded = std::clamp(
        rounded,
        0.0,
        static_cast<double>(Mask16::maximum));
    return static_cast<std::uint16_t>(bounded);
}

} // namespace

std::size_t Mask16::expected_sample_count() const {
    if (width == 0 || height == 0) {
        throw MaskError{"mask width and height must both be greater than zero"};
    }
    return checked_multiply(
        static_cast<std::size_t>(width),
        static_cast<std::size_t>(height));
}

void Mask16::validate() const {
    const auto expected = expected_sample_count();
    if (samples.size() != expected) {
        throw MaskError{
            "mask has " + std::to_string(samples.size()) +
            " samples but its dimensions require " + std::to_string(expected)};
    }
}

std::span<std::uint16_t> Mask16::row_span(const std::uint32_t y) {
    const auto offset = checked_row_offset(*this, y);
    return std::span<std::uint16_t>{samples}.subspan(
        offset,
        static_cast<std::size_t>(width));
}

std::span<const std::uint16_t> Mask16::row_span(
    const std::uint32_t y) const {
    const auto offset = checked_row_offset(*this, y);
    return std::span<const std::uint16_t>{samples}.subspan(
        offset,
        static_cast<std::size_t>(width));
}

std::uint16_t Mask16::sample(
    const std::uint32_t x,
    const std::uint32_t y) const {
    if (x >= width) {
        throw MaskError{"column coordinate is outside the mask"};
    }
    return row_span(y)[x];
}

float Mask16::coverage(
    const std::uint32_t x,
    const std::uint32_t y) const {
    return static_cast<float>(sample(x, y)) /
           static_cast<float>(maximum);
}

Mask16 Mask16::inverted() const {
    validate();
    Mask16 result{width, height, samples};
    for (auto& value : result.samples) {
        value = static_cast<std::uint16_t>(maximum - value);
    }
    return result;
}

Mask16 Mask16::with_strength(const float strength) const {
    validate();
    if (!std::isfinite(strength) || strength < 0.0F || strength > 1.0F) {
        throw MaskError{"mask strength must be finite and in [0, 1]"};
    }

    Mask16 result{width, height, samples};
    for (auto& value : result.samples) {
        const auto scaled =
            static_cast<double>(value) * static_cast<double>(strength);
        value = quantize_mask_sample(scaled);
    }
    return result;
}

std::string mask16_sha256(const Mask16& mask) {
    mask.validate();

    Sha256Context hash;
    hash.update(kMask16HashDomain.data(), kMask16HashDomain.size());

    const auto width = big_endian_uint32(mask.width);
    const auto height = big_endian_uint32(mask.height);
    hash.update(width.data(), width.size());
    hash.update(height.data(), height.size());

    std::array<unsigned char, kHashBufferBytes> buffer{};
    std::size_t used = 0U;
    for (const std::uint16_t sample : mask.samples) {
        buffer[used++] =
            static_cast<unsigned char>((sample >> 8U) & 0xFFU);
        buffer[used++] =
            static_cast<unsigned char>(sample & 0xFFU);
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
