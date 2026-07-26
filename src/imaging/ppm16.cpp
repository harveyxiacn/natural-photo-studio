#include "nps/imaging/ppm16.hpp"

#include <charconv>
#include <cstdint>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>

namespace nps::imaging {
namespace {

[[nodiscard]] unsigned char byte_at(
    const std::span<const std::byte> input,
    const std::size_t index) {
    return std::to_integer<unsigned char>(input[index]);
}

[[nodiscard]] bool is_ascii_whitespace(const unsigned char value) {
    switch (value) {
    case ' ':
    case '\t':
    case '\n':
    case '\r':
    case '\f':
    case '\v':
        return true;
    default:
        return false;
    }
}

void skip_header_trivia(
    const std::span<const std::byte> input,
    std::size_t& cursor) {
    while (cursor < input.size()) {
        if (is_ascii_whitespace(byte_at(input, cursor))) {
            ++cursor;
            continue;
        }

        if (byte_at(input, cursor) != '#') {
            return;
        }

        while (cursor < input.size()) {
            const auto value = byte_at(input, cursor++);
            if (value == '\n' || value == '\r') {
                break;
            }
        }
    }
}

[[nodiscard]] std::string_view read_header_token(
    const std::span<const std::byte> input,
    std::size_t& cursor) {
    skip_header_trivia(input, cursor);
    const auto start = cursor;

    while (cursor < input.size()) {
        const auto value = byte_at(input, cursor);
        if (is_ascii_whitespace(value) || value == '#') {
            break;
        }
        ++cursor;
    }

    if (cursor == start) {
        throw PpmError{"PPM header is missing a required token"};
    }

    const auto* characters =
        reinterpret_cast<const char*>(input.data() + start);
    return {characters, cursor - start};
}

[[nodiscard]] std::uint32_t parse_dimension(
    const std::string_view token,
    const char* field_name) {
    std::uint64_t value{};
    const auto parse_result =
        std::from_chars(token.data(), token.data() + token.size(), value);
    if (parse_result.ec != std::errc{} ||
        parse_result.ptr != token.data() + token.size() ||
        value == 0 ||
        value > std::numeric_limits<std::uint32_t>::max()) {
        throw PpmError{std::string{"invalid PPM "} + field_name};
    }
    return static_cast<std::uint32_t>(value);
}

[[nodiscard]] std::vector<std::byte> read_all_bytes(
    const std::filesystem::path& path) {
    std::ifstream stream{path, std::ios::binary | std::ios::ate};
    if (!stream) {
        throw PpmError{"unable to open PPM file for reading"};
    }

    const auto end = stream.tellg();
    const auto end_offset = static_cast<std::streamoff>(end);
    if (end_offset < 0) {
        throw PpmError{"unable to determine PPM file size"};
    }

    const auto unsigned_size = static_cast<std::uintmax_t>(end_offset);
    if (unsigned_size > std::numeric_limits<std::size_t>::max() ||
        unsigned_size >
            static_cast<std::uintmax_t>(
                std::numeric_limits<std::streamsize>::max())) {
        throw PpmError{"PPM file is too large to read"};
    }

    std::vector<std::byte> bytes(static_cast<std::size_t>(unsigned_size));
    stream.seekg(0, std::ios::beg);
    if (!bytes.empty()) {
        stream.read(
            reinterpret_cast<char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
        if (!stream || stream.gcount() != static_cast<std::streamsize>(bytes.size())) {
            throw PpmError{"unable to read the complete PPM file"};
        }
    }
    return bytes;
}

} // namespace

Image16 decode_ppm16(const std::span<const std::byte> encoded) {
    if (encoded.size() < 3 ||
        byte_at(encoded, 0) != 'P' ||
        byte_at(encoded, 1) != '6' ||
        !is_ascii_whitespace(byte_at(encoded, 2))) {
        throw PpmError{"PPM must begin with the P6 magic and a separator"};
    }

    std::size_t cursor = 2;
    const auto width = parse_dimension(read_header_token(encoded, cursor), "width");
    const auto height =
        parse_dimension(read_header_token(encoded, cursor), "height");
    const auto maxval = read_header_token(encoded, cursor);
    if (maxval != "65535") {
        throw PpmError{"16-bit PPM maxval must be exactly 65535"};
    }

    if (cursor >= encoded.size() ||
        !is_ascii_whitespace(byte_at(encoded, cursor))) {
        throw PpmError{"PPM maxval must be followed by one whitespace byte"};
    }
    ++cursor;

    Image16 image{width, height, {}};
    std::size_t expected_samples{};
    try {
        expected_samples = image.expected_sample_count();
    } catch (const ImageError& error) {
        throw PpmError{error.what()};
    }
    if (expected_samples > std::numeric_limits<std::size_t>::max() / 2) {
        throw PpmError{"PPM dimensions overflow the encoded payload size"};
    }
    const auto expected_bytes = expected_samples * 2;
    if (encoded.size() - cursor != expected_bytes) {
        throw PpmError{"PPM payload length does not match its dimensions"};
    }

    image.samples.resize(expected_samples);
    for (std::size_t sample_index = 0; sample_index < expected_samples;
         ++sample_index) {
        const auto encoded_index = cursor + sample_index * 2;
        const auto high = static_cast<std::uint16_t>(
            byte_at(encoded, encoded_index));
        const auto low = static_cast<std::uint16_t>(
            byte_at(encoded, encoded_index + 1));
        image.samples[sample_index] =
            static_cast<std::uint16_t>((high << 8U) | low);
    }

    return image;
}

std::vector<std::byte> encode_ppm16(const Image16& image) {
    image.validate();
    const std::string header =
        "P6\n" + std::to_string(image.width) + " " +
        std::to_string(image.height) + "\n65535\n";

    const auto sample_count = image.samples.size();
    if (sample_count >
        (std::numeric_limits<std::size_t>::max() - header.size()) / 2) {
        throw ImageError{"encoded PPM size overflows addressable memory"};
    }

    std::vector<std::byte> encoded;
    encoded.reserve(header.size() + sample_count * 2);
    for (const auto character : header) {
        encoded.push_back(
            static_cast<std::byte>(static_cast<unsigned char>(character)));
    }
    for (const auto sample : image.samples) {
        encoded.push_back(static_cast<std::byte>((sample >> 8U) & 0xffU));
        encoded.push_back(static_cast<std::byte>(sample & 0xffU));
    }
    return encoded;
}

Image16 read_ppm16_file(const std::filesystem::path& path) {
    const auto encoded = read_all_bytes(path);
    return decode_ppm16(encoded);
}

void write_ppm16_file(
    const std::filesystem::path& path,
    const Image16& image) {
    const auto encoded = encode_ppm16(image);
    std::ofstream stream{path, std::ios::binary | std::ios::trunc};
    if (!stream) {
        throw PpmError{"unable to open PPM file for writing"};
    }

    if (encoded.size() >
        static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max())) {
        throw PpmError{"encoded PPM is too large to write"};
    }
    stream.write(
        reinterpret_cast<const char*>(encoded.data()),
        static_cast<std::streamsize>(encoded.size()));
    if (!stream) {
        throw PpmError{"unable to write the complete PPM file"};
    }
}

} // namespace nps::imaging
