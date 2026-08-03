#include "nps/render/atomic_ppm_export.hpp"

#include "nps/imaging/ppm16.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace {

using nps::color::ColorEncoding;
using nps::render::AtomicPpmExportIdentity;
using nps::render::AtomicPpmExportError;
using nps::render::AtomicPpmExportErrorCode;
using nps::render::CancellationSource;
using nps::render::CancellationToken;
using nps::render::ExistingFilePolicy;

constexpr auto linear_srgb =
    ColorEncoding::scene_linear_srgb_d65;

class TemporaryDirectory final {
 public:
  TemporaryDirectory() {
    static std::atomic_uint64_t sequence{};
    const auto timestamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("nps-atomic-export-" + std::to_string(timestamp) + "-" +
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

  [[nodiscard]] const std::filesystem::path& path() const noexcept {
    return path_;
  }

 private:
  std::filesystem::path path_;
};

[[nodiscard]] nps::imaging::ImageF32 opaque_image() {
  return {
      2,
      1,
      linear_srgb,
      {
          0x1234 / 65535.0F,
          0xabcd / 65535.0F,
          1.0F,
          1.0F,
          0.0F,
          0.5F,
          0.25F,
          1.0F,
      }};
}

[[nodiscard]] nps::color::OpaqueImage16Options black_matte() {
  return {linear_srgb, {0.0F, 0.0F, 0.0F}};
}

[[nodiscard]] AtomicPpmExportIdentity valid_identity(
    const nps::imaging::ImageF32& image) {
  return {
      .document_id = "document-1",
      .snapshot_id = "snapshot-7",
      .revision = 7,
      .source_hash = std::string(64U, 'a'),
      .graph_hash = std::string(64U, 'b'),
      .color_id =
          std::string{nps::color::color_encoding_id(image.encoding)},
  };
}

void export_for_test(
    const nps::imaging::ImageF32& image,
    const std::filesystem::path& destination,
    const nps::color::OpaqueImage16Options& opaque_options,
    const ExistingFilePolicy policy =
        ExistingFilePolicy::refuse_existing,
    const std::span<const std::filesystem::path> forbidden_paths = {},
    const CancellationToken cancellation = {},
    const nps::render::AtomicPpmExportFaultPoint fault_point =
        nps::render::AtomicPpmExportFaultPoint::none) {
  TemporaryDirectory fallback_protected_scope;
  const std::array fallback_forbidden{
      fallback_protected_scope.path()};
  nps::render::export_atomic_ppm16(
      image,
      valid_identity(image),
      destination,
      opaque_options,
      forbidden_paths.empty()
          ? std::span<const std::filesystem::path>{fallback_forbidden}
          : forbidden_paths,
      policy,
      cancellation,
      fault_point);
}

[[nodiscard]] std::vector<std::byte> read_bytes(
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

void write_text(
    const std::filesystem::path& path,
    const std::string& value) {
  std::ofstream stream{path, std::ios::binary | std::ios::trunc};
  REQUIRE(stream);
  stream.write(value.data(), static_cast<std::streamsize>(value.size()));
  REQUIRE(stream);
}

[[nodiscard]] std::size_t temporary_export_count(
    const std::filesystem::path& directory) {
  std::size_t count{};
  for (const auto& entry :
       std::filesystem::directory_iterator{directory}) {
    const auto name = entry.path().filename().string();
    if (name.starts_with(".nps-export-") &&
        name.ends_with(".tmp")) {
      ++count;
    }
  }
  return count;
}

[[nodiscard]] AtomicPpmExportError capture_error(
    const auto& operation) {
  try {
    operation();
  } catch (const AtomicPpmExportError& error) {
    return error;
  }
  throw std::runtime_error{"expected AtomicPpmExportError"};
}

#ifdef _WIN32
[[nodiscard]] std::filesystem::path windows_namespace_path(
    const wchar_t separator,
    const wchar_t marker) {
  std::wstring native(2U, separator);
  native.push_back(marker);
  native.push_back(separator);
  if (marker == L'.') {
    native.append(L"PhysicalDrive0.ppm");
  } else {
    native.append(L"C:");
    native.push_back(separator);
    native.append(L"private.ppm");
  }
  return std::filesystem::path{native};
}

[[nodiscard]] std::filesystem::path windows_nt_namespace_path(
    const wchar_t separator) {
  std::wstring native(1U, separator);
  native.append(2U, L'?');
  native.push_back(separator);
  native.append(L"C:");
  native.push_back(separator);
  native.append(L"private.ppm");
  return std::filesystem::path{native};
}

[[nodiscard]] std::filesystem::path windows_network_path(
    const wchar_t separator) {
  std::wstring native(2U, separator);
  native.append(L"example-host");
  native.push_back(separator);
  native.append(L"share");
  native.push_back(separator);
  native.append(L"private.ppm");
  return std::filesystem::path{native};
}
#endif

}  // namespace

TEST_CASE("atomic PPM export creates canonical verified P6 output") {
  TemporaryDirectory temporary;
  const auto destination = temporary.path() / "new.ppm";
  const auto image = opaque_image();
  const auto expected =
      nps::color::premultiplied_f32_to_image16(image, black_matte());

  export_for_test(
      image,
      destination,
      black_matte());

  REQUIRE(nps::imaging::read_ppm16_file(destination) == expected);
  REQUIRE(
      read_bytes(destination) ==
      nps::imaging::encode_ppm16(expected));
  REQUIRE(temporary_export_count(temporary.path()) == 0);
}

TEST_CASE("atomic PPM export refuses an existing target by default") {
  TemporaryDirectory temporary;
  const auto destination = temporary.path() / "existing.ppm";
  write_text(destination, "original");
  const auto original = read_bytes(destination);

  const auto error = capture_error([&] {
    export_for_test(
        opaque_image(),
        destination,
        black_matte());
  });

  REQUIRE(error.code() == AtomicPpmExportErrorCode::destination_exists);
  REQUIRE(read_bytes(destination) == original);
  REQUIRE(temporary_export_count(temporary.path()) == 0);
}

TEST_CASE("atomic PPM export replaces only after verification") {
  TemporaryDirectory temporary;
  const auto destination = temporary.path() / "replace.ppm";
  write_text(destination, "old complete file");

  const auto image = opaque_image();
  const auto expected =
      nps::color::premultiplied_f32_to_image16(image, black_matte());
  export_for_test(
      image,
      destination,
      black_matte(),
      ExistingFilePolicy::replace_existing);

  REQUIRE(nps::imaging::read_ppm16_file(destination) == expected);
  REQUIRE(temporary_export_count(temporary.path()) == 0);
}

TEST_CASE("atomic PPM export uses the explicit matte for transparency") {
  TemporaryDirectory temporary;
  const auto destination = temporary.path() / "matte.ppm";
  const nps::imaging::ImageF32 transparent{
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

  export_for_test(
      transparent,
      destination,
      white);

  REQUIRE(
      nps::imaging::read_ppm16_file(destination).samples ==
      std::vector<std::uint16_t>{
          65535, 49151, 49151,
          65535, 65535, 65535});
}

TEST_CASE("atomic PPM export rejects invalid destinations without path leaks") {
  TemporaryDirectory temporary;

  SECTION("empty destination") {
    const auto error = capture_error([&] {
      export_for_test(
          opaque_image(),
          {},
          black_matte());
    });
    REQUIRE(
        error.code() ==
        AtomicPpmExportErrorCode::invalid_destination);
  }

  SECTION("non-PPM extension") {
    const auto destination = temporary.path() / "private-name.jpg";
    const auto error = capture_error([&] {
      export_for_test(
          opaque_image(),
          destination,
          black_matte());
    });
    REQUIRE(
        error.code() ==
        AtomicPpmExportErrorCode::invalid_destination);
    REQUIRE_FALSE(std::string{error.what()}.contains("private-name"));
  }

  REQUIRE(temporary_export_count(temporary.path()) == 0);
}

TEST_CASE("atomic PPM export rejects absolute lexical forbidden matches") {
  TemporaryDirectory temporary;
  const auto destination = temporary.path() / "reserved.ppm";
  write_text(destination, "authoritative project state");
  const auto original = read_bytes(destination);
  const std::vector<std::filesystem::path> forbidden{
      temporary.path() / "." / "reserved.ppm"};

  const auto error = capture_error([&] {
    export_for_test(
        opaque_image(),
        destination,
        black_matte(),
        ExistingFilePolicy::replace_existing,
        forbidden);
  });

  REQUIRE(
      error.code() ==
      AtomicPpmExportErrorCode::forbidden_destination);
  REQUIRE(read_bytes(destination) == original);
  REQUIRE_FALSE(std::string{error.what()}.contains("reserved.ppm"));
  REQUIRE(temporary_export_count(temporary.path()) == 0);
}

TEST_CASE("atomic PPM export rejects destinations below forbidden paths") {
  TemporaryDirectory temporary;
  const auto protected_root = temporary.path() / "project";
  const auto export_parent = protected_root / "exports";
  REQUIRE(std::filesystem::create_directories(export_parent));
  const auto destination = export_parent / "leak.ppm";
  const std::array forbidden{protected_root};

  const auto error = capture_error([&] {
    export_for_test(
        opaque_image(),
        destination,
        black_matte(),
        ExistingFilePolicy::replace_existing,
        forbidden);
  });

  REQUIRE(
      error.code() ==
      AtomicPpmExportErrorCode::forbidden_destination);
  REQUIRE_FALSE(std::filesystem::exists(destination));
  REQUIRE(temporary_export_count(export_parent) == 0);
}

TEST_CASE("atomic PPM export rejects existing filesystem aliases") {
  TemporaryDirectory temporary;
  const auto protected_file = temporary.path() / "project-state.db";
  const auto destination = temporary.path() / "state-alias.ppm";
  write_text(protected_file, "authoritative project state");

  std::error_code link_error;
  std::filesystem::create_hard_link(
      protected_file,
      destination,
      link_error);
  if (link_error) {
    WARN("platform did not permit a hard-link fixture");
    return;
  }
  const auto original = read_bytes(protected_file);
  const std::array forbidden{protected_file};

  const auto error = capture_error([&] {
    export_for_test(
        opaque_image(),
        destination,
        black_matte(),
        ExistingFilePolicy::replace_existing,
        forbidden);
  });

  REQUIRE(
      error.code() ==
      AtomicPpmExportErrorCode::forbidden_destination);
  REQUIRE(read_bytes(protected_file) == original);
  REQUIRE(read_bytes(destination) == original);
  REQUIRE(temporary_export_count(temporary.path()) == 0);
}

TEST_CASE(
    "atomic PPM export resolves forbidden directory aliases before "
    "descendant checks") {
  TemporaryDirectory temporary;
  const auto protected_root = temporary.path() / "project-real";
  const auto protected_alias = temporary.path() / "project-alias";
  const auto export_parent = protected_root / "exports";
  REQUIRE(std::filesystem::create_directories(export_parent));

  std::error_code link_error;
  std::filesystem::create_directory_symlink(
      protected_root,
      protected_alias,
      link_error);
  if (link_error) {
    WARN("platform did not permit a forbidden-directory alias fixture");
    return;
  }

  const auto destination = export_parent / "leak.ppm";
  const std::array forbidden{protected_alias};
  const auto error = capture_error([&] {
    export_for_test(
        opaque_image(),
        destination,
        black_matte(),
        ExistingFilePolicy::replace_existing,
        forbidden);
  });

  REQUIRE(
      error.code() ==
      AtomicPpmExportErrorCode::forbidden_destination);
  REQUIRE_FALSE(std::filesystem::exists(destination));
  REQUIRE(temporary_export_count(export_parent) == 0);
}

TEST_CASE("forbidden path containment does not reject sibling prefixes") {
  TemporaryDirectory temporary;
  const auto protected_root = temporary.path() / "project";
  const auto export_parent = temporary.path() / "project-public";
  REQUIRE(std::filesystem::create_directory(protected_root));
  REQUIRE(std::filesystem::create_directory(export_parent));
  const auto destination = export_parent / "allowed.ppm";
  const std::array forbidden{protected_root};

  export_for_test(
      opaque_image(),
      destination,
      black_matte(),
      ExistingFilePolicy::replace_existing,
      forbidden);

  REQUIRE(std::filesystem::exists(destination));
  REQUIRE(temporary_export_count(export_parent) == 0);
}

TEST_CASE("atomic PPM export requires a complete snapshot identity") {
  TemporaryDirectory temporary;
  const auto destination = temporary.path() / "identity.ppm";
  const auto image = opaque_image();
  const std::array forbidden{
      temporary.path() / ".nps-protected-project"};

  const auto reject = [&](const AtomicPpmExportIdentity& identity) {
    return capture_error([&] {
      nps::render::export_atomic_ppm16(
          image,
          identity,
          destination,
          black_matte(),
          forbidden);
    });
  };

  SECTION("document ID") {
    auto identity = valid_identity(image);
    identity.document_id = "../private";
    REQUIRE(
        reject(identity).code() ==
        AtomicPpmExportErrorCode::invalid_input);
  }
  SECTION("snapshot ID") {
    auto identity = valid_identity(image);
    identity.snapshot_id.clear();
    REQUIRE(
        reject(identity).code() ==
        AtomicPpmExportErrorCode::invalid_input);
  }
  SECTION("revision") {
    auto identity = valid_identity(image);
    identity.revision = -1;
    REQUIRE(
        reject(identity).code() ==
        AtomicPpmExportErrorCode::invalid_input);
  }
  SECTION("source hash") {
    auto identity = valid_identity(image);
    identity.source_hash = std::string(64U, 'A');
    REQUIRE(
        reject(identity).code() ==
        AtomicPpmExportErrorCode::invalid_input);
  }
  SECTION("graph hash") {
    auto identity = valid_identity(image);
    identity.graph_hash = "short";
    REQUIRE(
        reject(identity).code() ==
        AtomicPpmExportErrorCode::invalid_input);
  }
  SECTION("unsupported color ID") {
    auto identity = valid_identity(image);
    identity.color_id = "nps.color/unknown/v1";
    REQUIRE(
        reject(identity).code() ==
        AtomicPpmExportErrorCode::invalid_input);
  }
  SECTION("mismatched image encoding") {
    auto identity = valid_identity(image);
    identity.color_id = std::string{
        nps::color::scene_linear_rec2020_d65_id};
    REQUIRE(
        reject(identity).code() ==
        AtomicPpmExportErrorCode::invalid_input);
  }

  REQUIRE_FALSE(std::filesystem::exists(destination));
  REQUIRE(temporary_export_count(temporary.path()) == 0);
}

TEST_CASE("atomic PPM export requires a non-empty forbidden scope") {
  TemporaryDirectory temporary;
  const auto image = opaque_image();
  const auto destination = temporary.path() / "unscoped.ppm";

  const auto error = capture_error([&] {
    nps::render::export_atomic_ppm16(
        image,
        valid_identity(image),
        destination,
        black_matte(),
        std::span<const std::filesystem::path>{});
  });

  REQUIRE(error.code() == AtomicPpmExportErrorCode::invalid_input);
  REQUIRE_FALSE(std::filesystem::exists(destination));
  REQUIRE(temporary_export_count(temporary.path()) == 0);

  const std::array forbidden_with_empty{
      std::filesystem::path{}};
  const auto empty_entry_error = capture_error([&] {
    nps::render::export_atomic_ppm16(
        image,
        valid_identity(image),
        destination,
        black_matte(),
        forbidden_with_empty);
  });
  REQUIRE(
      empty_entry_error.code() ==
      AtomicPpmExportErrorCode::invalid_input);
}

TEST_CASE(
    "atomic PPM export rejects a missing forbidden path before matching") {
  TemporaryDirectory temporary;
  const auto protected_root = temporary.path() / "project";
  REQUIRE(std::filesystem::create_directory(protected_root));
  const auto destination = protected_root / "missing-scope.ppm";
  const auto image = opaque_image();
  const std::array forbidden{
      protected_root,
      temporary.path() / "missing-private-state"};

  const auto error = capture_error([&] {
    nps::render::export_atomic_ppm16(
        image,
        valid_identity(image),
        destination,
        black_matte(),
        forbidden,
        ExistingFilePolicy::replace_existing);
  });

  REQUIRE(error.code() == AtomicPpmExportErrorCode::invalid_input);
  REQUIRE_FALSE(
      std::string{error.what()}.contains("missing-private-state"));
  REQUIRE_FALSE(std::filesystem::exists(destination));
  REQUIRE(temporary_export_count(protected_root) == 0);
}

TEST_CASE("atomic PPM export rejects embedded NUL before native path use") {
  TemporaryDirectory temporary;
  const auto image = opaque_image();
  const std::array ordinary_forbidden{
      temporary.path() / ".nps-protected-project"};

  SECTION("destination") {
    auto native = (temporary.path() / "truncated").native();
    native.push_back(std::filesystem::path::value_type{});
    const auto suffix = std::filesystem::path{".ppm"}.native();
    native.append(suffix);
    const std::filesystem::path destination{native};

    auto deliberately_invalid_identity = valid_identity(image);
    deliberately_invalid_identity.document_id.clear();
    const auto error = capture_error([&] {
      nps::render::export_atomic_ppm16(
          image,
          deliberately_invalid_identity,
          destination,
          black_matte(),
          ordinary_forbidden);
    });
    REQUIRE(
        error.code() ==
        AtomicPpmExportErrorCode::invalid_destination);
    REQUIRE_FALSE(std::filesystem::exists(
        temporary.path() / "truncated"));
  }

  SECTION("forbidden path") {
    auto native =
        (temporary.path() / ".nps-protected-project").native();
    native.push_back(std::filesystem::path::value_type{});
    native.push_back(
        static_cast<std::filesystem::path::value_type>('x'));
    const std::array forbidden{std::filesystem::path{native}};
    const auto destination = temporary.path() / "nul-forbidden.ppm";

    auto deliberately_invalid_identity = valid_identity(image);
    deliberately_invalid_identity.document_id.clear();
    const auto error = capture_error([&] {
      nps::render::export_atomic_ppm16(
          image,
          deliberately_invalid_identity,
          destination,
          black_matte(),
          forbidden);
    });
    REQUIRE(error.code() == AtomicPpmExportErrorCode::invalid_input);
    REQUIRE_FALSE(std::filesystem::exists(destination));
  }

  REQUIRE(temporary_export_count(temporary.path()) == 0);
}

#ifdef _WIN32
TEST_CASE("atomic PPM export rejects Windows path aliases") {
  TemporaryDirectory temporary;
  const auto image = opaque_image();
  const std::array forbidden{
      temporary.path() / ".nps-protected-project"};

  const auto reject = [&](const std::filesystem::path& destination) {
    return capture_error([&] {
      nps::render::export_atomic_ppm16(
          image,
          valid_identity(image),
          destination,
          black_matte(),
          forbidden);
    });
  };

  REQUIRE(
      reject(temporary.path() / "NUL.ppm").code() ==
      AtomicPpmExportErrorCode::unsafe_destination);
  REQUIRE(
      reject(temporary.path() / "NUL .ppm").code() ==
      AtomicPpmExportErrorCode::unsafe_destination);
  REQUIRE(
      reject(temporary.path() / "stream.ppm:private").code() ==
      AtomicPpmExportErrorCode::unsafe_destination);
  REQUIRE(
      reject(temporary.path() / "directory." / "out.ppm").code() ==
      AtomicPpmExportErrorCode::unsafe_destination);
  REQUIRE(
      reject(windows_namespace_path(L'/', L'?')).code() ==
      AtomicPpmExportErrorCode::unsafe_destination);
  REQUIRE(
      reject(windows_namespace_path(L'\\', L'?')).code() ==
      AtomicPpmExportErrorCode::unsafe_destination);
  REQUIRE(
      reject(windows_namespace_path(L'/', L'.')).code() ==
      AtomicPpmExportErrorCode::unsafe_destination);
  REQUIRE(
      reject(windows_namespace_path(L'\\', L'.')).code() ==
      AtomicPpmExportErrorCode::unsafe_destination);
  REQUIRE(
      reject(windows_nt_namespace_path(L'\\')).code() ==
      AtomicPpmExportErrorCode::unsafe_destination);
  REQUIRE(
      reject(windows_network_path(L'/')).code() ==
      AtomicPpmExportErrorCode::unsafe_destination);
  REQUIRE(
      reject(windows_network_path(L'\\')).code() ==
      AtomicPpmExportErrorCode::unsafe_destination);
  REQUIRE(temporary_export_count(temporary.path()) == 0);
}

TEST_CASE("Windows replacement cannot inherit old alternate streams") {
  TemporaryDirectory temporary;
  const auto destination = temporary.path() / "replace-stream.ppm";
  write_text(destination, "old target");
  auto stream_native = destination.native();
  stream_native.append(L":private");
  const std::filesystem::path alternate_stream{stream_native};
  write_text(alternate_stream, "must not survive");

  export_for_test(
      opaque_image(),
      destination,
      black_matte(),
      ExistingFilePolicy::replace_existing);

  std::ifstream retained_stream{
      alternate_stream,
      std::ios::binary};
  REQUIRE_FALSE(retained_stream.is_open());
  REQUIRE(temporary_export_count(temporary.path()) == 0);
}
#endif

TEST_CASE("atomic PPM export cancellation leaves target untouched") {
  TemporaryDirectory temporary;
  const auto destination = temporary.path() / "cancelled.ppm";
  write_text(destination, "old target");
  const auto original = read_bytes(destination);
  CancellationSource cancellation;
  cancellation.request_stop();

  const auto error = capture_error([&] {
    export_for_test(
        opaque_image(),
        destination,
        black_matte(),
        ExistingFilePolicy::replace_existing,
        {},
        cancellation.get_token());
  });

  REQUIRE(error.code() == AtomicPpmExportErrorCode::cancelled);
  REQUIRE(read_bytes(destination) == original);
  REQUIRE(temporary_export_count(temporary.path()) == 0);
}

TEST_CASE("temporary write failure preserves target and leaves no residue") {
  TemporaryDirectory temporary;
  const auto destination = temporary.path() / "write-failure.ppm";
  write_text(destination, "old target");
  const auto original = read_bytes(destination);

  const auto error = capture_error([&] {
    export_for_test(
        opaque_image(),
        destination,
        black_matte(),
        ExistingFilePolicy::replace_existing,
        {},
        {},
        nps::render::AtomicPpmExportFaultPoint::after_first_write);
  });

  REQUIRE(error.code() == AtomicPpmExportErrorCode::io_failure);
  REQUIRE(read_bytes(destination) == original);
  REQUIRE(temporary_export_count(temporary.path()) == 0);
}

TEST_CASE("atomic PPM export handles platform path aliases safely") {
  TemporaryDirectory temporary;
  const auto real_parent = temporary.path() / "real";
  const auto linked_parent = temporary.path() / "linked";
  REQUIRE(std::filesystem::create_directory(real_parent));

  std::error_code link_error;
  std::filesystem::create_directory_symlink(
      real_parent,
      linked_parent,
      link_error);
  if (link_error) {
    WARN("platform did not permit a directory symlink fixture");
    return;
  }

#ifdef _WIN32
  const auto parent_error = capture_error([&] {
    export_for_test(
        opaque_image(),
        linked_parent / "unsafe.ppm",
        black_matte());
  });

  REQUIRE(
      parent_error.code() ==
      AtomicPpmExportErrorCode::unsafe_destination);
  REQUIRE_FALSE(std::filesystem::exists(real_parent / "unsafe.ppm"));
#else
  // POSIX resolves the already-existing parent to its canonical directory.
  // This is required for macOS system aliases such as /var -> /private/var.
  export_for_test(
      opaque_image(),
      linked_parent / "safe.ppm",
      black_matte());
  REQUIRE(std::filesystem::exists(real_parent / "safe.ppm"));
#endif
  REQUIRE(temporary_export_count(real_parent) == 0);

  const auto real_target = temporary.path() / "real-target.ppm";
  const auto linked_target = temporary.path() / "linked-target.ppm";
  write_text(real_target, "source file");
  std::filesystem::create_symlink(
      real_target,
      linked_target,
      link_error);
  if (link_error) {
    WARN("platform did not permit a file symlink fixture");
    return;
  }
  const auto target_error = capture_error([&] {
    export_for_test(
        opaque_image(),
        linked_target,
        black_matte(),
        ExistingFilePolicy::replace_existing);
  });
  REQUIRE(
      target_error.code() ==
      AtomicPpmExportErrorCode::unsafe_destination);
  REQUIRE(read_bytes(real_target) == read_bytes(linked_target));
  REQUIRE(temporary_export_count(temporary.path()) == 0);
}
