#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace nps::imaging {

class MaskError : public std::invalid_argument {
public:
    using std::invalid_argument::invalid_argument;
};

// Single-channel selection coverage: 0 is excluded and 65535 is fully
// selected. Storage is row-major with exactly width * height samples.
struct Mask16 final {
    static constexpr std::uint16_t maximum = 65535;

    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<std::uint16_t> samples{};

    [[nodiscard]] std::size_t expected_sample_count() const;
    void validate() const;

    [[nodiscard]] std::span<std::uint16_t> row_span(std::uint32_t y);
    [[nodiscard]] std::span<const std::uint16_t> row_span(
        std::uint32_t y) const;
    [[nodiscard]] std::uint16_t sample(
        std::uint32_t x,
        std::uint32_t y) const;
    [[nodiscard]] float coverage(
        std::uint32_t x,
        std::uint32_t y) const;

    // Inversion is exact: output = 65535 - input.
    [[nodiscard]] Mask16 inverted() const;

    // Strength must be finite and in [0, 1]. Each sample is multiplied by
    // strength and quantized with round-half-up.
    [[nodiscard]] Mask16 with_strength(float strength) const;

    friend bool operator==(const Mask16&, const Mask16&) = default;
};

// Returns the lowercase SHA-256 of the validated canonical mask stream.
//
// The stream starts with the ASCII bytes
// "nps.mask16/canonical-sha256/v1" (without a terminator), then contains
// big-endian uint32 width and height followed by row-major big-endian uint16
// coverage samples.
[[nodiscard]] std::string mask16_sha256(const Mask16& mask);

} // namespace nps::imaging
