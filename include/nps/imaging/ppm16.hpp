#pragma once

#include "nps/imaging/image16.hpp"

#include <cstddef>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <vector>

namespace nps::imaging {

class PpmError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Decodes a strict binary P6 PPM whose maxval is exactly 65535. Samples in the
// file are interpreted as big-endian, as required by the PPM specification.
[[nodiscard]] Image16 decode_ppm16(std::span<const std::byte> encoded);

// Produces a canonical P6 header with LF delimiters and big-endian samples.
[[nodiscard]] std::vector<std::byte> encode_ppm16(const Image16& image);

[[nodiscard]] Image16 read_ppm16_file(const std::filesystem::path& path);
void write_ppm16_file(const std::filesystem::path& path, const Image16& image);

} // namespace nps::imaging
