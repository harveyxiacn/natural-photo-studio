#include "nps/imaging/image16.hpp"
#include "nps/imaging/operations.hpp"
#include "nps/imaging/ppm16.hpp"
#include "nps/imaging/synthetic.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace {

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        static std::atomic_uint64_t sequence{};
        const auto timestamp = std::chrono::steady_clock::now()
                                   .time_since_epoch()
                                   .count();
        path_ = std::filesystem::temp_directory_path() /
                ("nps-imaging-" + std::to_string(timestamp) + "-" +
                 std::to_string(sequence.fetch_add(1)));
        if (!std::filesystem::create_directory(path_)) {
            throw std::runtime_error{"unable to create test directory"};
        }
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const {
        return path_;
    }

private:
    std::filesystem::path path_;
};

[[nodiscard]] std::vector<std::byte> bytes_from_ascii(
    const std::string& text) {
    std::vector<std::byte> bytes;
    bytes.reserve(text.size());
    for (const auto character : text) {
        bytes.push_back(
            static_cast<std::byte>(static_cast<unsigned char>(character)));
    }
    return bytes;
}

[[nodiscard]] std::vector<std::byte> read_file(
    const std::filesystem::path& path) {
    std::ifstream stream{path, std::ios::binary};
    REQUIRE(stream);
    const std::vector<char> characters{
        std::istreambuf_iterator<char>{stream},
        std::istreambuf_iterator<char>{}};
    std::vector<std::byte> bytes;
    bytes.reserve(characters.size());
    for (const auto character : characters) {
        bytes.push_back(
            static_cast<std::byte>(static_cast<unsigned char>(character)));
    }
    return bytes;
}

} // namespace

TEST_CASE("one positive EV doubles linear samples") {
    const nps::imaging::Image16 source{
        2,
        1,
        {0, 1, 32767, 32768, 40000, 65535}};

    const auto adjusted = nps::imaging::apply_exposure(source, 1.0);

    REQUIRE(adjusted.width == 2);
    REQUIRE(adjusted.height == 1);
    REQUIRE(
        adjusted.samples ==
        std::vector<std::uint16_t>{0, 2, 65534, 65535, 65535, 65535});
    REQUIRE(source.samples[3] == 32768);
}

TEST_CASE("exposure clamps instead of wrapping") {
    const nps::imaging::Image16 source{1, 1, {1, 32768, 65535}};

    const auto adjusted = nps::imaging::apply_exposure(source, 32.0);

    REQUIRE(
        adjusted.samples ==
        std::vector<std::uint16_t>{65535, 65535, 65535});
}

TEST_CASE("imaging operations reject invalid values and dimensions") {
    const nps::imaging::Image16 wrong_sample_count{2, 1, {0, 0, 0}};
    REQUIRE_THROWS_AS(
        nps::imaging::encode_ppm16(wrong_sample_count),
        nps::imaging::ImageError);
    REQUIRE_THROWS_AS(
        nps::imaging::make_deterministic_gradient(0, 1),
        nps::imaging::ImageError);

    const nps::imaging::Image16 source{1, 1, {0, 1, 65535}};
    REQUIRE_THROWS_AS(
        nps::imaging::apply_exposure(
            source,
            std::numeric_limits<double>::infinity()),
        nps::imaging::ImageError);
}

TEST_CASE("canonical PPM round trips through memory and disk") {
    const auto source = nps::imaging::make_deterministic_gradient(17, 9);
    const auto encoded = nps::imaging::encode_ppm16(source);

    REQUIRE(nps::imaging::decode_ppm16(encoded) == source);

    TemporaryDirectory temporary;
    const auto path = temporary.path() / "gradient.ppm";
    nps::imaging::write_ppm16_file(path, source);
    REQUIRE(nps::imaging::read_ppm16_file(path) == source);
    REQUIRE(read_file(path) == encoded);
}

TEST_CASE("decoder rejects malformed or ambiguous input") {
    auto wrong_magic = bytes_from_ascii("P3\n1 1\n65535\n");
    wrong_magic.insert(wrong_magic.end(), 6, std::byte{0});
    REQUIRE_THROWS_AS(
        nps::imaging::decode_ppm16(wrong_magic),
        nps::imaging::PpmError);

    auto wrong_maxval = bytes_from_ascii("P6\n1 1\n255\n");
    wrong_maxval.insert(wrong_maxval.end(), 6, std::byte{0});
    REQUIRE_THROWS_AS(
        nps::imaging::decode_ppm16(wrong_maxval),
        nps::imaging::PpmError);

    auto truncated = bytes_from_ascii("P6\n1 1\n65535\n");
    truncated.insert(truncated.end(), 5, std::byte{0});
    REQUIRE_THROWS_AS(
        nps::imaging::decode_ppm16(truncated),
        nps::imaging::PpmError);

    auto trailing = bytes_from_ascii("P6\n1 1\n65535\n");
    trailing.insert(trailing.end(), 7, std::byte{0});
    REQUIRE_THROWS_AS(
        nps::imaging::decode_ppm16(trailing),
        nps::imaging::PpmError);

    const auto overflowing =
        bytes_from_ascii("P6\n4294967295 4294967295\n65535\n");
    REQUIRE_THROWS_AS(
        nps::imaging::decode_ppm16(overflowing),
        nps::imaging::PpmError);
}

TEST_CASE("canonical output is byte-identical and big-endian") {
    const nps::imaging::Image16 source{1, 1, {0x1234, 0xabcd, 0xffff}};

    auto expected = bytes_from_ascii("P6\n1 1\n65535\n");
    expected.insert(
        expected.end(),
        {
            std::byte{0x12},
            std::byte{0x34},
            std::byte{0xab},
            std::byte{0xcd},
            std::byte{0xff},
            std::byte{0xff},
        });

    const auto first = nps::imaging::encode_ppm16(source);
    const auto second = nps::imaging::encode_ppm16(source);
    REQUIRE(first == expected);
    REQUIRE(second == first);
}
