#include "nps/render/atomic_ppm_export.hpp"

#include "nps/imaging/image16.hpp"
#include "nps/imaging/ppm16.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <openssl/rand.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#if defined(__APPLE__)
#include <stdio.h>
#endif
#endif

namespace nps::render {
namespace {

[[noreturn]] void fail(
    const AtomicPpmExportErrorCode code,
    const char* message) {
  throw AtomicPpmExportError{code, message};
}

void check_cancelled(const std::stop_token stop_token) {
  if (stop_token.stop_requested()) {
    fail(
        AtomicPpmExportErrorCode::cancelled,
        "PPM export was cancelled");
  }
}

[[nodiscard]] bool path_contains_nul(
    const std::filesystem::path& path) noexcept {
  const auto& native = path.native();
  return native.find(std::filesystem::path::value_type{}) !=
         std::filesystem::path::string_type::npos;
}

void validate_raw_path_inputs(
    const std::filesystem::path& destination,
    const std::span<const std::filesystem::path> forbidden_paths) {
  // This must precede extension parsing, normalization, native c_str() calls,
  // and even identity validation. Native APIs otherwise truncate at the NUL
  // and can inspect or publish a different path than the validated one.
  if (path_contains_nul(destination)) {
    fail(
        AtomicPpmExportErrorCode::invalid_destination,
        "the export destination contains an invalid character");
  }
  for (const auto& forbidden : forbidden_paths) {
    if (path_contains_nul(forbidden)) {
      fail(
          AtomicPpmExportErrorCode::invalid_input,
          "a forbidden export path contains an invalid character");
    }
  }
  if (forbidden_paths.empty()) {
    fail(
        AtomicPpmExportErrorCode::invalid_input,
        "at least one forbidden export path is required");
  }
  if (std::ranges::any_of(
          forbidden_paths,
          [](const auto& path) { return path.empty(); })) {
    fail(
        AtomicPpmExportErrorCode::invalid_input,
        "forbidden export paths must be non-empty");
  }
}

[[nodiscard]] bool is_ascii_alphanumeric(const char value) noexcept {
  return (value >= 'a' && value <= 'z') ||
         (value >= 'A' && value <= 'Z') ||
         (value >= '0' && value <= '9');
}

[[nodiscard]] bool is_stable_identifier(
    const std::string_view value) noexcept {
  return !value.empty() && value.size() <= 128U &&
         is_ascii_alphanumeric(value.front()) &&
         std::ranges::all_of(value, [](const char character) {
           return is_ascii_alphanumeric(character) ||
                  character == '.' || character == '_' ||
                  character == ':' || character == '-';
         });
}

[[nodiscard]] bool is_lower_sha256(
    const std::string_view value) noexcept {
  return value.size() == 64U &&
         std::ranges::all_of(value, [](const char character) {
           return (character >= '0' && character <= '9') ||
                  (character >= 'a' && character <= 'f');
         });
}

void validate_identity(
    const AtomicPpmExportIdentity& identity,
    const imaging::ImageF32& image) {
  if (!is_stable_identifier(identity.document_id) ||
      !is_stable_identifier(identity.snapshot_id)) {
    fail(
        AtomicPpmExportErrorCode::invalid_input,
        "the export document and snapshot IDs must be stable identifiers");
  }
  if (identity.revision < 0) {
    fail(
        AtomicPpmExportErrorCode::invalid_input,
        "the export revision cannot be negative");
  }
  if (!is_lower_sha256(identity.source_hash) ||
      !is_lower_sha256(identity.graph_hash)) {
    fail(
        AtomicPpmExportErrorCode::invalid_input,
        "the export hashes must be lowercase SHA-256 values");
  }
  const auto encoding =
      color::color_encoding_from_id(identity.color_id);
  if (!encoding.has_value() || *encoding != image.encoding) {
    fail(
        AtomicPpmExportErrorCode::invalid_input,
        "the export color identity must match the image encoding");
  }
}

#ifdef _WIN32
[[nodiscard]] bool is_windows_separator(
    const wchar_t character) noexcept {
  return character == L'\\' || character == L'/';
}

[[nodiscard]] bool is_windows_unc_path(
    const std::filesystem::path& path) noexcept {
  const auto& native = path.native();
  return native.size() >= 2U &&
         is_windows_separator(native[0U]) &&
         is_windows_separator(native[1U]);
}

[[nodiscard]] wchar_t ascii_upper(const wchar_t character) noexcept {
  if (character >= L'a' && character <= L'z') {
    return static_cast<wchar_t>(character - L'a' + L'A');
  }
  return character;
}

[[nodiscard]] bool is_reserved_windows_device_name(
    const std::wstring_view component) noexcept {
  const auto dot = component.find(L'.');
  auto base = component.substr(0U, dot);
  while (!base.empty() &&
         (base.back() == L'.' || base.back() == L' ')) {
    base.remove_suffix(1U);
  }
  const auto equals_ascii_case_insensitive =
      [](const std::wstring_view left,
         const std::wstring_view right) noexcept {
        return left.size() == right.size() &&
               std::ranges::equal(
                   left,
                   right,
                   [](const wchar_t actual, const wchar_t expected) {
                     return ascii_upper(actual) == expected;
                   });
      };

  if (equals_ascii_case_insensitive(base, L"CON") ||
      equals_ascii_case_insensitive(base, L"PRN") ||
      equals_ascii_case_insensitive(base, L"AUX") ||
      equals_ascii_case_insensitive(base, L"NUL") ||
      equals_ascii_case_insensitive(base, L"CLOCK$")) {
    return true;
  }
  if (base.size() != 4U ||
      (!equals_ascii_case_insensitive(base.substr(0U, 3U), L"COM") &&
       !equals_ascii_case_insensitive(base.substr(0U, 3U), L"LPT"))) {
    return false;
  }
  const auto suffix = ascii_upper(base.back());
  return (suffix >= L'1' && suffix <= L'9') ||
         suffix == L'\u00b9' || suffix == L'\u00b2' ||
         suffix == L'\u00b3';
}

void validate_windows_path_spelling(
    const std::filesystem::path& path) {
  const std::wstring_view native{path.native()};
  const auto is_native_namespace =
      native.size() >= 4U && is_windows_separator(native[0U]) &&
      ((is_windows_separator(native[1U]) &&
        (native[2U] == L'.' || native[2U] == L'?') &&
        is_windows_separator(native[3U])) ||
       (native[1U] == L'?' && native[2U] == L'?' &&
        is_windows_separator(native[3U])));
  if (is_native_namespace) {
    fail(
        AtomicPpmExportErrorCode::unsafe_destination,
        "Windows device and extended path namespaces are not exportable");
  }

  for (const auto& component_path : path.relative_path()) {
    const auto& component = component_path.native();
    if (component.empty()) {
      continue;
    }
    if (component.find(L':') != std::wstring::npos) {
      fail(
          AtomicPpmExportErrorCode::unsafe_destination,
          "Windows alternate data streams are not exportable");
    }
    if (component.back() == L'.' || component.back() == L' ') {
      fail(
          AtomicPpmExportErrorCode::unsafe_destination,
          "Windows export path components may not end in dot or space");
    }
    if (is_reserved_windows_device_name(component)) {
      fail(
          AtomicPpmExportErrorCode::unsafe_destination,
          "Windows device aliases are not exportable");
    }
  }
}

void validate_windows_destination_location(
    const std::filesystem::path& path) {
  if (is_windows_unc_path(path)) {
    fail(
        AtomicPpmExportErrorCode::unsafe_destination,
        "network export destinations are not supported");
  }

  const auto root = path.root_path();
  if (!root.empty() && GetDriveTypeW(root.c_str()) == DRIVE_REMOTE) {
    fail(
        AtomicPpmExportErrorCode::unsafe_destination,
        "network export destinations are not supported");
  }
}
#endif

[[nodiscard]] std::filesystem::path normalize_absolute_path(
    const std::filesystem::path& path,
    const AtomicPpmExportErrorCode error_code) {
  std::error_code error;
  auto absolute = std::filesystem::absolute(path, error);
  if (error || absolute.empty()) {
    fail(error_code, "unable to normalize an export path");
  }
  return absolute.lexically_normal();
}

[[nodiscard]] bool lexical_paths_equal(
    const std::filesystem::path& left,
    const std::filesystem::path& right) noexcept {
#ifdef _WIN32
  return CompareStringOrdinal(
             left.c_str(),
             -1,
             right.c_str(),
             -1,
             TRUE) == CSTR_EQUAL;
#else
  return left == right;
#endif
}

[[nodiscard]] bool lexical_path_is_equal_or_descendant(
    const std::filesystem::path& candidate,
    const std::filesystem::path& protected_path) noexcept {
  auto candidate_component = candidate.begin();
  for (auto protected_component = protected_path.begin();
       protected_component != protected_path.end();
       ++protected_component, ++candidate_component) {
    auto next_protected = protected_component;
    ++next_protected;
    if (protected_component->empty() &&
        next_protected == protected_path.end()) {
      return true;
    }
    if (candidate_component == candidate.end() ||
        !lexical_paths_equal(
            *candidate_component,
            *protected_component)) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool is_missing_path_error(
    const std::error_code& error) noexcept {
  return error == std::errc::no_such_file_or_directory ||
         error == std::errc::not_a_directory;
}

[[nodiscard]] bool existing_path_is_equal_or_descendant(
    const std::filesystem::path& candidate,
    const std::filesystem::path& protected_path) {
  std::error_code error;
  const auto protected_status =
      std::filesystem::status(protected_path, error);
  if (error || !std::filesystem::exists(protected_status)) {
    fail(
        AtomicPpmExportErrorCode::invalid_input,
        "every forbidden export path must resolve to an existing object");
  }

  for (auto current = candidate; !current.empty();) {
    error.clear();
    const auto current_status = std::filesystem::status(current, error);
    if (!error && std::filesystem::exists(current_status)) {
      error.clear();
      if (std::filesystem::equivalent(
              current,
              protected_path,
              error)) {
        return true;
      }
      if (error) {
        fail(
            AtomicPpmExportErrorCode::unsafe_destination,
            "unable to compare an export path safely");
      }
    } else if (error && !is_missing_path_error(error)) {
      fail(
          AtomicPpmExportErrorCode::unsafe_destination,
          "unable to inspect an export path safely");
    }

    const auto parent = current.parent_path();
    if (parent == current) {
      break;
    }
    current = parent;
  }
  return false;
}

#ifndef _WIN32
[[nodiscard]] std::filesystem::path canonicalize_existing_parent(
    const std::filesystem::path& destination) {
  std::error_code error;
  const auto parent =
      std::filesystem::canonical(destination.parent_path(), error);
  if (error || parent.empty()) {
    fail(
        AtomicPpmExportErrorCode::invalid_destination,
        "unable to resolve the export parent");
  }
  return (parent / destination.filename()).lexically_normal();
}

[[nodiscard]] std::filesystem::path canonicalize_for_comparison(
    const std::filesystem::path& path) {
  std::error_code error;
  const auto canonical =
      std::filesystem::canonical(path, error);
  if (error || canonical.empty()) {
    fail(
        AtomicPpmExportErrorCode::invalid_input,
        "unable to resolve a forbidden export path");
  }
  return canonical.lexically_normal();
}
#endif

enum class InspectedPathKind {
  missing,
  regular_file,
  directory,
};

[[nodiscard]] InspectedPathKind inspect_path_component(
    const std::filesystem::path& path,
    const bool may_be_missing) {
#ifdef _WIN32
  const auto attributes = GetFileAttributesW(path.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    const auto error = GetLastError();
    if (may_be_missing &&
        (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)) {
      return InspectedPathKind::missing;
    }
    fail(
        AtomicPpmExportErrorCode::io_failure,
        "unable to inspect the export destination");
  }
  if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
    fail(
        AtomicPpmExportErrorCode::unsafe_destination,
        "export paths may not contain reparse points");
  }
  if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0U) {
    return InspectedPathKind::directory;
  }
  if ((attributes & FILE_ATTRIBUTE_DEVICE) != 0U) {
    fail(
        AtomicPpmExportErrorCode::unsafe_destination,
        "the export destination must be an ordinary file");
  }
  return InspectedPathKind::regular_file;
#else
  std::error_code error;
  const auto status = std::filesystem::symlink_status(path, error);
  if (error) {
    if (may_be_missing &&
        error == std::errc::no_such_file_or_directory) {
      return InspectedPathKind::missing;
    }
    fail(
        AtomicPpmExportErrorCode::io_failure,
        "unable to inspect the export destination");
  }
  if (status.type() == std::filesystem::file_type::not_found &&
      may_be_missing) {
    return InspectedPathKind::missing;
  }
  if (std::filesystem::is_symlink(status)) {
    fail(
        AtomicPpmExportErrorCode::unsafe_destination,
        "export paths may not contain symbolic links");
  }
  if (std::filesystem::is_directory(status)) {
    return InspectedPathKind::directory;
  }
  if (!std::filesystem::is_regular_file(status)) {
    fail(
        AtomicPpmExportErrorCode::unsafe_destination,
        "the export destination must be an ordinary file");
  }
  return InspectedPathKind::regular_file;
#endif
}

[[nodiscard]] bool validate_destination_safety(
    const std::filesystem::path& destination) {
  std::vector<std::filesystem::path> ancestors;
  for (auto current = destination.parent_path(); !current.empty();) {
    ancestors.push_back(current);
    const auto parent = current.parent_path();
    if (parent == current) {
      break;
    }
    current = parent;
  }
  std::reverse(ancestors.begin(), ancestors.end());

  for (const auto& ancestor : ancestors) {
    if (inspect_path_component(ancestor, false) !=
        InspectedPathKind::directory) {
      fail(
          AtomicPpmExportErrorCode::invalid_destination,
          "the export parent must be a directory");
    }
  }

  const auto destination_kind =
      inspect_path_component(destination, true);
  if (destination_kind == InspectedPathKind::directory) {
    fail(
        AtomicPpmExportErrorCode::invalid_destination,
        "the export destination must not be a directory");
  }
  return destination_kind == InspectedPathKind::regular_file;
}

[[nodiscard]] std::filesystem::path validate_request_paths(
    const std::filesystem::path& destination,
    const ExistingFilePolicy policy,
    const std::span<const std::filesystem::path> forbidden_paths) {
  if (forbidden_paths.empty()) {
    fail(
        AtomicPpmExportErrorCode::invalid_input,
        "at least one forbidden export path is required");
  }
#ifdef _WIN32
  validate_windows_path_spelling(destination);
  validate_windows_destination_location(destination);
#endif
  if (destination.empty() || destination.filename().empty() ||
      destination.extension() != ".ppm") {
    fail(
        AtomicPpmExportErrorCode::invalid_destination,
        "the export destination must have a .ppm extension");
  }
  if (policy != ExistingFilePolicy::refuse_existing &&
      policy != ExistingFilePolicy::replace_existing) {
    fail(
        AtomicPpmExportErrorCode::invalid_input,
        "the existing-file policy is invalid");
  }

  auto normalized = normalize_absolute_path(
      destination,
      AtomicPpmExportErrorCode::invalid_destination);
#ifndef _WIN32
  // Resolve only the existing parent, not the destination filename. This
  // permits benign system aliases such as /var while keeping an existing
  // destination symlink visible to the final lstat-style inspection.
  normalized = canonicalize_existing_parent(normalized);
#else
  validate_windows_path_spelling(normalized);
  validate_windows_destination_location(normalized);
#endif
  std::vector<std::filesystem::path> normalized_forbidden_paths;
  normalized_forbidden_paths.reserve(forbidden_paths.size());
  for (const auto& forbidden : forbidden_paths) {
    if (forbidden.empty()) {
      fail(
          AtomicPpmExportErrorCode::invalid_input,
          "forbidden export paths must be non-empty");
    }
    auto normalized_forbidden = normalize_absolute_path(
        forbidden,
        AtomicPpmExportErrorCode::invalid_input);
#ifndef _WIN32
    normalized_forbidden =
        canonicalize_for_comparison(normalized_forbidden);
#else
    validate_windows_path_spelling(normalized_forbidden);
    std::error_code status_error;
    const auto forbidden_status =
        std::filesystem::status(normalized_forbidden, status_error);
    if (status_error || !std::filesystem::exists(forbidden_status)) {
      fail(
          AtomicPpmExportErrorCode::invalid_input,
          "every forbidden export path must resolve to an existing object");
    }
#endif
    normalized_forbidden_paths.push_back(
        std::move(normalized_forbidden));
  }

  for (const auto& normalized_forbidden :
       normalized_forbidden_paths) {
    if (lexical_path_is_equal_or_descendant(
            normalized,
            normalized_forbidden) ||
        existing_path_is_equal_or_descendant(
            normalized,
            normalized_forbidden)) {
      fail(
          AtomicPpmExportErrorCode::forbidden_destination,
          "the export destination is reserved by the project");
    }
  }

  const auto exists = validate_destination_safety(normalized);
  if (exists && policy == ExistingFilePolicy::refuse_existing) {
    fail(
        AtomicPpmExportErrorCode::destination_exists,
        "the export destination already exists");
  }
  return normalized;
}

[[nodiscard]] std::string random_suffix() {
  std::array<unsigned char, 16> bytes{};
  if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) {
    fail(
        AtomicPpmExportErrorCode::io_failure,
        "unable to generate a private temporary export name");
  }

  constexpr std::array<char, 16> hexadecimal{
      '0', '1', '2', '3', '4', '5', '6', '7',
      '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
  std::string suffix;
  suffix.reserve(bytes.size() * 2);
  for (const auto byte : bytes) {
    suffix.push_back(hexadecimal[byte >> 4U]);
    suffix.push_back(hexadecimal[byte & 0x0fU]);
  }
  return suffix;
}

#ifdef _WIN32
using NativeFileHandle = HANDLE;
const NativeFileHandle invalid_file_handle = INVALID_HANDLE_VALUE;
#else
using NativeFileHandle = int;
constexpr NativeFileHandle invalid_file_handle = -1;
#endif

class OwnedTemporaryFile final {
 public:
  OwnedTemporaryFile(
      std::filesystem::path path,
      const NativeFileHandle handle)
      : path_(std::move(path)), handle_(handle) {}

  OwnedTemporaryFile(const OwnedTemporaryFile&) = delete;
  OwnedTemporaryFile& operator=(const OwnedTemporaryFile&) = delete;

  OwnedTemporaryFile(OwnedTemporaryFile&& other) noexcept
      : path_(std::move(other.path_)),
        handle_(std::exchange(other.handle_, invalid_file_handle)) {
    other.path_.clear();
  }

  OwnedTemporaryFile& operator=(OwnedTemporaryFile&&) = delete;

  ~OwnedTemporaryFile() {
    close_noexcept();
    if (!path_.empty()) {
      std::error_code ignored;
      std::filesystem::remove(path_, ignored);
    }
  }

  [[nodiscard]] const std::filesystem::path& path() const noexcept {
    return path_;
  }

  void write_all(
      const std::span<const std::byte> bytes,
      const std::stop_token stop_token,
      const AtomicPpmExportFaultPoint fault_point) {
    constexpr std::size_t maximum_chunk = 64U * 1024U;
    std::size_t written{};
    bool first_write_completed = false;
    while (written < bytes.size()) {
      check_cancelled(stop_token);
      const auto chunk_size =
          std::min(maximum_chunk, bytes.size() - written);
#ifdef _WIN32
      DWORD native_written{};
      const auto succeeded = WriteFile(
          handle_,
          bytes.data() + written,
          static_cast<DWORD>(chunk_size),
          &native_written,
          nullptr);
      if (succeeded == FALSE ||
          native_written != static_cast<DWORD>(chunk_size)) {
        fail(
            AtomicPpmExportErrorCode::io_failure,
            "unable to write the complete temporary export");
      }
      written += native_written;
#else
      const auto result = ::write(
          handle_,
          bytes.data() + written,
          chunk_size);
      if (result < 0) {
        if (errno == EINTR) {
          continue;
        }
        fail(
            AtomicPpmExportErrorCode::io_failure,
            "unable to write the complete temporary export");
      }
      if (result == 0) {
        fail(
            AtomicPpmExportErrorCode::io_failure,
            "unable to write the complete temporary export");
      }
      written += static_cast<std::size_t>(result);
#endif
      if (!first_write_completed) {
        first_write_completed = true;
        if (fault_point ==
            AtomicPpmExportFaultPoint::after_first_write) {
          fail(
              AtomicPpmExportErrorCode::io_failure,
              "injected temporary export write failure");
        }
      }
    }
    check_cancelled(stop_token);
  }

  void flush_and_close() {
#ifdef _WIN32
    if (FlushFileBuffers(handle_) == FALSE) {
      fail(
          AtomicPpmExportErrorCode::io_failure,
          "unable to flush the temporary export");
    }
    const auto handle = std::exchange(handle_, invalid_file_handle);
    if (CloseHandle(handle) == FALSE) {
      fail(
          AtomicPpmExportErrorCode::io_failure,
          "unable to close the temporary export");
    }
#else
#if defined(__APPLE__) && defined(F_FULLFSYNC)
    int sync_result{};
    do {
      sync_result = ::fcntl(handle_, F_FULLFSYNC);
    } while (sync_result != 0 && errno == EINTR);
    if (sync_result != 0 && errno != EINVAL && errno != ENOTSUP) {
      fail(
          AtomicPpmExportErrorCode::io_failure,
          "unable to fully synchronize the temporary export");
    }
    if (sync_result != 0) {
      do {
        sync_result = ::fsync(handle_);
      } while (sync_result != 0 && errno == EINTR);
    }
#else
    int sync_result{};
    do {
      sync_result = ::fsync(handle_);
    } while (sync_result != 0 && errno == EINTR);
#endif
    if (sync_result != 0) {
      fail(
          AtomicPpmExportErrorCode::io_failure,
          "unable to flush the temporary export");
    }
    const auto handle = std::exchange(handle_, invalid_file_handle);
    if (::close(handle) != 0) {
      fail(
          AtomicPpmExportErrorCode::io_failure,
          "unable to close the temporary export");
    }
#endif
  }

  void release() noexcept { path_.clear(); }

 private:
  void close_noexcept() noexcept {
    if (handle_ == invalid_file_handle) {
      return;
    }
#ifdef _WIN32
    static_cast<void>(CloseHandle(handle_));
#else
    static_cast<void>(::close(handle_));
#endif
    handle_ = invalid_file_handle;
  }

  std::filesystem::path path_;
  NativeFileHandle handle_{invalid_file_handle};
};

[[nodiscard]] OwnedTemporaryFile create_temporary_file(
    const std::filesystem::path& parent) {
  constexpr std::size_t maximum_attempts = 128;
  for (std::size_t attempt = 0; attempt < maximum_attempts; ++attempt) {
    const auto candidate =
        parent / (".nps-export-" + random_suffix() + ".tmp");
#ifdef _WIN32
    const auto handle = CreateFileW(
        candidate.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (handle != INVALID_HANDLE_VALUE) {
      return OwnedTemporaryFile{candidate, handle};
    }
    if (GetLastError() == ERROR_FILE_EXISTS ||
        GetLastError() == ERROR_ALREADY_EXISTS) {
      continue;
    }
#else
    const auto handle = ::open(
        candidate.c_str(),
        O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
        S_IRUSR | S_IWUSR);
    if (handle >= 0) {
      return OwnedTemporaryFile{candidate, handle};
    }
    if (errno == EEXIST) {
      continue;
    }
#endif
    fail(
        AtomicPpmExportErrorCode::io_failure,
        "unable to create a temporary export");
  }
  fail(
      AtomicPpmExportErrorCode::io_failure,
      "unable to reserve a unique temporary export");
}

#ifndef _WIN32
void sync_parent_before_publish(const std::filesystem::path& parent) {
  const auto directory =
      ::open(parent.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY);
  if (directory < 0) {
    fail(
        AtomicPpmExportErrorCode::io_failure,
        "unable to open the export parent for synchronization");
  }

  int result{};
  do {
    result = ::fsync(directory);
  } while (result != 0 && errno == EINTR);
  const auto sync_error = result != 0;
  static_cast<void>(::close(directory));
  if (sync_error) {
    fail(
        AtomicPpmExportErrorCode::io_failure,
        "unable to synchronize the export parent");
  }
}

void sync_parent_after_publish_noexcept(
    const std::filesystem::path& parent) noexcept {
  const auto directory =
      ::open(parent.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY);
  if (directory < 0) {
    return;
  }
  while (::fsync(directory) != 0 && errno == EINTR) {
  }
  static_cast<void>(::close(directory));
}
#endif

void publish_no_replace(
    OwnedTemporaryFile& temporary,
    const std::filesystem::path& destination) {
#ifdef _WIN32
  if (MoveFileExW(
          temporary.path().c_str(),
          destination.c_str(),
          MOVEFILE_WRITE_THROUGH) == FALSE) {
    if (GetLastError() == ERROR_FILE_EXISTS ||
        GetLastError() == ERROR_ALREADY_EXISTS) {
      fail(
          AtomicPpmExportErrorCode::destination_exists,
          "the export destination already exists");
    }
    fail(
        AtomicPpmExportErrorCode::publish_failed,
        "unable to publish the verified export");
  }
#elif defined(__linux__) && defined(SYS_renameat2)
  constexpr unsigned int rename_no_replace = 1U;
  if (::syscall(
          SYS_renameat2,
          AT_FDCWD,
          temporary.path().c_str(),
          AT_FDCWD,
          destination.c_str(),
          rename_no_replace) != 0) {
    if (errno == EEXIST) {
      fail(
          AtomicPpmExportErrorCode::destination_exists,
          "the export destination already exists");
    }
    fail(
        AtomicPpmExportErrorCode::publish_failed,
        "unable to publish the verified export");
  }
#elif defined(__APPLE__)
  if (::renamex_np(
          temporary.path().c_str(),
          destination.c_str(),
          RENAME_EXCL) != 0) {
    if (errno == EEXIST) {
      fail(
          AtomicPpmExportErrorCode::destination_exists,
          "the export destination already exists");
    }
    fail(
        AtomicPpmExportErrorCode::publish_failed,
        "unable to publish the verified export");
  }
#else
  if (::link(temporary.path().c_str(), destination.c_str()) != 0) {
    if (errno == EEXIST) {
      fail(
          AtomicPpmExportErrorCode::destination_exists,
          "the export destination already exists");
    }
    fail(
        AtomicPpmExportErrorCode::publish_failed,
        "unable to publish the verified export");
  }
  if (::unlink(temporary.path().c_str()) != 0) {
    // The destination already names the complete verified inode. Do not turn a
    // successful publication into a false failure; the owner destructor makes
    // one more best-effort attempt to remove the private temporary hard link.
    return;
  }
#endif
  temporary.release();
}

void publish_replace(
    OwnedTemporaryFile& temporary,
    const std::filesystem::path& destination,
    const bool destination_existed) {
#ifdef _WIN32
  static_cast<void>(destination_existed);
  // ReplaceFileW can report ERROR_UNABLE_TO_MOVE_REPLACEMENT after it has
  // already removed or renamed the old target. Treating that as an ordinary
  // failure lets the temporary-file destructor delete the only complete copy.
  // It also merges named streams from the old target into the replacement.
  // A same-volume MoveFileEx rename avoids both recovery hazards.
  if (MoveFileExW(
          temporary.path().c_str(),
          destination.c_str(),
          MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE) {
    fail(
        AtomicPpmExportErrorCode::publish_failed,
        "unable to replace the export destination");
  }
#else
  static_cast<void>(destination_existed);
  if (::rename(temporary.path().c_str(), destination.c_str()) != 0) {
    fail(
        AtomicPpmExportErrorCode::publish_failed,
        "unable to replace the export destination");
  }
#endif
  temporary.release();
}

}  // namespace

std::string_view to_string(
    const AtomicPpmExportErrorCode code) noexcept {
  switch (code) {
    case AtomicPpmExportErrorCode::invalid_input:
      return "invalid_input";
    case AtomicPpmExportErrorCode::invalid_destination:
      return "invalid_destination";
    case AtomicPpmExportErrorCode::unsafe_destination:
      return "unsafe_destination";
    case AtomicPpmExportErrorCode::forbidden_destination:
      return "forbidden_destination";
    case AtomicPpmExportErrorCode::destination_exists:
      return "destination_exists";
    case AtomicPpmExportErrorCode::cancelled:
      return "cancelled";
    case AtomicPpmExportErrorCode::io_failure:
      return "io_failure";
    case AtomicPpmExportErrorCode::verification_failed:
      return "verification_failed";
    case AtomicPpmExportErrorCode::publish_failed:
      return "publish_failed";
  }
  return "unknown";
}

AtomicPpmExportError::AtomicPpmExportError(
    const AtomicPpmExportErrorCode code,
    std::string message)
    : std::runtime_error(std::move(message)), code_(code) {}

AtomicPpmExportErrorCode AtomicPpmExportError::code() const noexcept {
  return code_;
}

void export_atomic_ppm16(
    const imaging::ImageF32& image,
    const AtomicPpmExportIdentity& identity,
    const std::filesystem::path& destination,
    const color::OpaqueImage16Options& opaque_options,
    const std::span<const std::filesystem::path> forbidden_paths,
    const ExistingFilePolicy existing_file_policy,
    const std::stop_token stop_token,
    const AtomicPpmExportFaultPoint fault_point) {
  validate_raw_path_inputs(destination, forbidden_paths);
  validate_identity(identity, image);
  check_cancelled(stop_token);
  switch (fault_point) {
    case AtomicPpmExportFaultPoint::none:
    case AtomicPpmExportFaultPoint::after_first_write:
    case AtomicPpmExportFaultPoint::hard_exit_after_flush:
      break;
    default:
      fail(
          AtomicPpmExportErrorCode::invalid_input,
          "the export fault point is invalid");
  }
  const auto normalized_destination = validate_request_paths(
      destination,
      existing_file_policy,
      forbidden_paths);

  imaging::Image16 expected;
  std::vector<std::byte> encoded;
  try {
    expected = color::premultiplied_f32_to_image16(
        image,
        opaque_options);
    encoded = imaging::encode_ppm16(expected);
  } catch (const AtomicPpmExportError&) {
    throw;
  } catch (const std::exception&) {
    fail(
        AtomicPpmExportErrorCode::invalid_input,
        "the image or opaque conversion options are invalid");
  }
  check_cancelled(stop_token);

  auto temporary =
      create_temporary_file(normalized_destination.parent_path());
  temporary.write_all(encoded, stop_token, fault_point);
  temporary.flush_and_close();
  if (fault_point ==
      AtomicPpmExportFaultPoint::hard_exit_after_flush) {
    std::_Exit(kAtomicPpmExportHardExitCode);
  }
  check_cancelled(stop_token);

  try {
    const auto decoded = imaging::read_ppm16_file(temporary.path());
    if (decoded != expected) {
      fail(
          AtomicPpmExportErrorCode::verification_failed,
          "the temporary export failed pixel verification");
    }
  } catch (const AtomicPpmExportError&) {
    throw;
  } catch (const std::exception&) {
    fail(
        AtomicPpmExportErrorCode::verification_failed,
        "the temporary export failed strict PPM verification");
  }
  check_cancelled(stop_token);

  // Recheck link/reparse and existing-target state immediately before the
  // native publication primitive. The primitive itself enforces no-replace
  // atomically for refuse_existing.
  const auto destination_existed =
      validate_destination_safety(normalized_destination);
  if (destination_existed &&
      existing_file_policy == ExistingFilePolicy::refuse_existing) {
    fail(
        AtomicPpmExportErrorCode::destination_exists,
        "the export destination already exists");
  }

#ifndef _WIN32
  sync_parent_before_publish(normalized_destination.parent_path());
#endif
  // The final cooperative cancellation gate is deliberately adjacent to the
  // native commit primitive. A request arriving after this point races with an
  // indivisible OS publication and therefore observes either complete file.
  check_cancelled(stop_token);
  if (existing_file_policy == ExistingFilePolicy::refuse_existing) {
    publish_no_replace(temporary, normalized_destination);
  } else {
    publish_replace(
        temporary,
        normalized_destination,
        destination_existed);
  }
#ifndef _WIN32
  // Publication has already succeeded. A directory-fsync failure cannot be
  // rolled back without violating atomic replacement, so this final durability
  // reinforcement is deliberately best effort.
  sync_parent_after_publish_noexcept(
      normalized_destination.parent_path());
#endif
}

}  // namespace nps::render
