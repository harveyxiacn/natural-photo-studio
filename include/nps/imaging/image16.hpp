#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace nps::imaging {

class ImageError : public std::invalid_argument {
public:
    using std::invalid_argument::invalid_argument;
};

// Linear-light RGB image with three interleaved 16-bit channels per pixel.
struct Image16 final {
    static constexpr std::size_t channel_count = 3;

    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<std::uint16_t> samples{};

    [[nodiscard]] std::size_t expected_sample_count() const;
    void validate() const;

    friend bool operator==(const Image16&, const Image16&) = default;
};

} // namespace nps::imaging
