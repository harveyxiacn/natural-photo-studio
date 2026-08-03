#include "nps/core/project_store.hpp"
#include "nps/document/edit_graph.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <set>
#include <sstream>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <variant>

#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <sqlite3.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace nps::core {
namespace {

constexpr int kProjectApplicationId = 0x4E505331;
constexpr int kProjectUserVersionV1 = 1;
constexpr int kProjectUserVersionV2 = 2;
constexpr int kInjectedCrashExitCode = 86;
constexpr std::size_t kSha256Bytes = 32;
constexpr std::size_t kSha256HexCharacters = kSha256Bytes * 2;
constexpr std::size_t kMaximumEditGraphJsonBytes =
    4U * 1024U * 1024U;
constexpr std::uintmax_t kMaximumObjectBytes =
    static_cast<std::uintmax_t>(8) * 1024U * 1024U * 1024U;
constexpr std::string_view kProjectFormatV1 = "nps.project/v1";
constexpr std::string_view kProjectFormatV2 = "nps.project/v2";
constexpr std::string_view kEditGraphSchema = "nps.edit-graph/v1";
constexpr std::string_view kWorkingColorId =
    "nps.color/scene-linear-rec2020-d65/v1";

[[nodiscard]] std::string path_to_utf8(const std::filesystem::path& path) {
  const auto bytes = path.generic_u8string();
  return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

[[noreturn]] void throw_error(std::string code, std::string message) {
  throw ProjectStoreError(std::move(code), std::move(message));
}

[[nodiscard]] std::string random_identifier(std::string_view prefix) {
  std::array<unsigned char, 16> random_bytes{};
  if (RAND_bytes(
          random_bytes.data(),
          static_cast<int>(random_bytes.size())) != 1) {
    throw_error(
        "INTERNAL_RANDOM",
        "A cryptographically secure project identifier could not be generated.");
  }
  constexpr std::array<char, 16> hex{
      '0', '1', '2', '3', '4', '5', '6', '7',
      '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
  std::string value(prefix);
  value.reserve(prefix.size() + random_bytes.size() * 2U);
  for (const unsigned char byte : random_bytes) {
    value.push_back(hex[static_cast<std::size_t>(byte >> 4U)]);
    value.push_back(hex[static_cast<std::size_t>(byte & 0x0FU)]);
  }
  return value;
}

[[nodiscard]] bool is_lower_hex(std::string_view value) {
  if (value.size() != kSha256HexCharacters) {
    return false;
  }
  for (const char character : value) {
    const bool digit = character >= '0' && character <= '9';
    const bool lower = character >= 'a' && character <= 'f';
    if (!digit && !lower) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool is_link_or_reparse_point(
    const std::filesystem::path& path,
    const std::filesystem::file_status& status) {
  if (std::filesystem::is_symlink(status)) {
    return true;
  }
#ifdef _WIN32
  const DWORD attributes = GetFileAttributesW(path.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    throw_error("IO_OBJECT_STAT", "A project path could not be inspected.");
  }
  return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U;
#else
  static_cast<void>(path);
  return false;
#endif
}

void require_real_directory(const std::filesystem::path& path) {
  std::error_code error;
  const auto status = std::filesystem::symlink_status(path, error);
  if (error || !std::filesystem::is_directory(status) ||
      is_link_or_reparse_point(path, status)) {
    throw_error(
        "IO_OBJECT_LINK",
        "Project object directories must be real directories.");
  }
}

void require_real_directory_ancestry(const std::filesystem::path& path) {
  if (!path.is_absolute()) {
    throw_error(
        "IO_PROJECT_PATH",
        "A migration path could not be resolved safely.");
  }
  std::filesystem::path current = path.root_path();
  for (const auto& component : path.relative_path()) {
    current /= component;
    require_real_directory(current);
  }
}

[[nodiscard]] bool is_missing_path_error(
    const std::error_code& error) noexcept {
  return error == std::errc::no_such_file_or_directory ||
         error == std::errc::not_a_directory;
}

[[nodiscard]] bool path_entry_exists_no_follow(
    const std::filesystem::path& path,
    std::string_view error_code,
    std::string_view error_message) {
  std::error_code error;
  const auto status = std::filesystem::symlink_status(path, error);
  if (error) {
    if (is_missing_path_error(error)) {
      return false;
    }
    throw_error(std::string(error_code), std::string(error_message));
  }
  return status.type() != std::filesystem::file_type::not_found;
}

[[nodiscard]] std::filesystem::path resolve_project_root(
    const std::filesystem::path& project_root,
    std::string_view error_message) {
  std::error_code error;
  std::filesystem::path resolved =
      std::filesystem::absolute(project_root, error);
  if (error || resolved.empty()) {
    throw_error("IO_PROJECT_PATH", std::string(error_message));
  }
#ifdef _WIN32
  resolved = resolved.lexically_normal();
#else
  // Resolve only the existing parent. The final .npsproj component must stay
  // unresolved so the caller's lstat-style check can reject a project-root
  // symlink while benign system aliases such as /var -> /private/var do not
  // reach SQLite's SQLITE_OPEN_NOFOLLOW path-component rejection. Do not
  // lexically collapse the POSIX path first: link/.. must be evaluated after
  // link resolution, as it is by the filesystem.
  const std::filesystem::path canonical_parent =
      std::filesystem::canonical(resolved.parent_path(), error);
  if (error || canonical_parent.empty()) {
    throw_error("IO_PROJECT_PATH", std::string(error_message));
  }
  resolved = canonical_parent / resolved.filename();
#endif
  return resolved;
}

[[nodiscard]] std::string sha256(std::span<const std::uint8_t> bytes) {
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned int digest_size = 0;
  EVP_MD_CTX* context = EVP_MD_CTX_new();
  if (context == nullptr) {
    throw_error("INTERNAL_HASH", "Unable to allocate the SHA-256 context.");
  }

  const auto release_context = [&context]() {
    EVP_MD_CTX_free(context);
    context = nullptr;
  };

  if (EVP_DigestInit_ex(context, EVP_sha256(), nullptr) != 1 ||
      EVP_DigestUpdate(context, bytes.data(), bytes.size()) != 1 ||
      EVP_DigestFinal_ex(context, digest.data(), &digest_size) != 1) {
    release_context();
    throw_error("INTERNAL_HASH", "Unable to calculate the object hash.");
  }
  release_context();

  if (digest_size != kSha256Bytes) {
    throw_error("INTERNAL_HASH", "The SHA-256 provider returned an invalid size.");
  }

  constexpr std::array<char, 16> hex{
      '0', '1', '2', '3', '4', '5', '6', '7',
      '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
  std::string result;
  result.reserve(kSha256HexCharacters);
  for (std::size_t index = 0; index < kSha256Bytes; ++index) {
    const unsigned char value = digest[index];
    result.push_back(hex[static_cast<std::size_t>(value >> 4U)]);
    result.push_back(hex[static_cast<std::size_t>(value & 0x0FU)]);
  }
  return result;
}

[[nodiscard]] std::string sha256(std::string_view text) {
  return sha256(std::span<const std::uint8_t>{
      reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
}

[[nodiscard]] std::vector<std::uint8_t> read_file(
    const std::filesystem::path& path) {
  std::error_code error;
  const auto link_status = std::filesystem::symlink_status(path, error);
  if (error || !std::filesystem::is_regular_file(link_status) ||
      is_link_or_reparse_point(path, link_status)) {
    throw_error("IO_OBJECT_MISSING", "A required project object is unavailable.");
  }

  const std::uintmax_t file_size = std::filesystem::file_size(path, error);
  if (error || file_size > kMaximumObjectBytes ||
      file_size > static_cast<std::uintmax_t>(
                      std::numeric_limits<std::size_t>::max())) {
    throw_error("IO_OBJECT_SIZE", "A project object has an invalid size.");
  }

  const auto size = static_cast<std::size_t>(file_size);
  std::vector<std::uint8_t> bytes(size);
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    throw_error("IO_OBJECT_READ", "A project object could not be opened.");
  }
  if (size > 0U) {
    if (size > static_cast<std::size_t>(
                   std::numeric_limits<std::streamsize>::max())) {
      throw_error("IO_OBJECT_SIZE", "A project object is too large to read.");
    }
    stream.read(
        reinterpret_cast<char*>(bytes.data()),
        static_cast<std::streamsize>(size));
    if (!stream) {
      throw_error("IO_OBJECT_READ", "A project object could not be read.");
    }
  }
  return bytes;
}

void sync_file(const std::filesystem::path& path) {
#ifdef _WIN32
  int descriptor = -1;
  const errno_t open_result = _wsopen_s(
      &descriptor,
      path.c_str(),
      _O_RDWR | _O_BINARY,
      _SH_DENYNO,
      _S_IREAD);
  if (open_result != 0 || descriptor == -1) {
    throw_error("IO_OBJECT_SYNC", "A staged project object could not be synced.");
  }
  const int result = _commit(descriptor);
  const int close_result = _close(descriptor);
#else
  const int descriptor = ::open(path.c_str(), O_RDWR);
  if (descriptor == -1) {
    throw_error("IO_OBJECT_SYNC", "A staged project object could not be synced.");
  }
  const int result = ::fsync(descriptor);
  const int close_result = ::close(descriptor);
#endif
  if (result != 0 || close_result != 0) {
    throw_error("IO_OBJECT_SYNC", "A staged project object could not be synced.");
  }
}

[[nodiscard]] std::filesystem::path object_path(
    const std::filesystem::path& project_root,
    std::string_view hash) {
  if (!is_lower_hex(hash)) {
    throw_error("IO_OBJECT_HASH", "A project object hash is invalid.");
  }
  return project_root / "objects" / "sha256" /
         std::string(hash.substr(0, 2)) / std::string(hash.substr(2));
}

void validate_object_ancestry(
    const std::filesystem::path& project_root,
    std::string_view hash) {
  if (!is_lower_hex(hash)) {
    throw_error("IO_OBJECT_HASH", "A project object hash is invalid.");
  }
  require_real_directory(project_root);
  require_real_directory(project_root / "objects");
  require_real_directory(project_root / "objects" / "sha256");
  require_real_directory(
      project_root / "objects" / "sha256" /
      std::string(hash.substr(0, 2)));
}

[[nodiscard]] std::string persist_object(
    const std::filesystem::path& project_root,
    std::span<const std::uint8_t> bytes) {
  if (bytes.size() > kMaximumObjectBytes) {
    throw_error("IO_OBJECT_SIZE", "The source object exceeds the M0 size limit.");
  }

  const std::string hash = sha256(bytes);
  const std::filesystem::path destination = object_path(project_root, hash);
  std::error_code error;
  if (std::filesystem::exists(destination, error)) {
    if (error) {
      throw_error("IO_OBJECT_STAT", "An existing object could not be inspected.");
    }
    validate_object_ancestry(project_root, hash);
    const auto existing = read_file(destination);
    if (sha256(existing) != hash) {
      throw_error("IO_OBJECT_CORRUPT", "An existing object failed verification.");
    }
    return hash;
  }

  const std::filesystem::path staging_directory =
      project_root / "objects" / ".staging";
  std::filesystem::create_directories(destination.parent_path(), error);
  if (error) {
    throw_error("IO_PROJECT_CREATE", "The object directory could not be created.");
  }
  std::filesystem::create_directories(staging_directory, error);
  if (error) {
    throw_error("IO_PROJECT_CREATE", "The staging directory could not be created.");
  }
  validate_object_ancestry(project_root, hash);

  static std::atomic<std::uint64_t> sequence{0};
  const std::filesystem::path staged =
      staging_directory /
      (hash + ".tmp-" + std::to_string(sequence.fetch_add(1)));
  {
    std::ofstream stream(staged, std::ios::binary | std::ios::trunc);
    if (!stream) {
      throw_error("IO_OBJECT_WRITE", "The staged object could not be opened.");
    }
    if (!bytes.empty()) {
      if (bytes.size() > static_cast<std::size_t>(
                             std::numeric_limits<std::streamsize>::max())) {
        throw_error("IO_OBJECT_SIZE", "The source object is too large to write.");
      }
      stream.write(
          reinterpret_cast<const char*>(bytes.data()),
          static_cast<std::streamsize>(bytes.size()));
    }
    stream.flush();
    if (!stream) {
      throw_error("IO_OBJECT_WRITE", "The staged object could not be written.");
    }
  }
  sync_file(staged);

  std::filesystem::rename(staged, destination, error);
  if (error) {
    if (std::filesystem::exists(destination)) {
      std::filesystem::remove(staged);
    } else {
      throw_error("IO_OBJECT_COMMIT", "The staged object could not be committed.");
    }
  }

  const auto persisted = read_file(destination);
  if (sha256(persisted) != hash) {
    throw_error("IO_OBJECT_CORRUPT", "The committed object failed verification.");
  }
  return hash;
}

void inject_crash_if_requested(
    FaultPoint configured,
    FaultPoint reached) noexcept {
  if (configured == reached) {
    std::_Exit(kInjectedCrashExitCode);
  }
}

class ProjectLock final {
 public:
  explicit ProjectLock(const std::filesystem::path& lock_path) {
#ifdef _WIN32
    handle_ = CreateFileW(
        lock_path.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr);
    if (handle_ == INVALID_HANDLE_VALUE) {
      throw_error(
          "IO_PROJECT_LOCKED",
          "The project is already open or its lock cannot be acquired.");
    }
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (GetFileInformationByHandleEx(
            handle_,
            FileAttributeTagInfo,
            &attributes,
            static_cast<DWORD>(sizeof(attributes))) == 0 ||
        (attributes.FileAttributes &
         (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY |
          FILE_ATTRIBUTE_DEVICE)) != 0U) {
      CloseHandle(handle_);
      handle_ = INVALID_HANDLE_VALUE;
      throw_error(
          "IO_PROJECT_LOCK",
          "The project lock file is not a regular local file.");
    }
#else
    int flags = O_CREAT | O_RDWR;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    descriptor_ = ::open(lock_path.c_str(), flags, S_IRUSR | S_IWUSR);
    if (descriptor_ == -1 ||
        ::flock(descriptor_, LOCK_EX | LOCK_NB) != 0) {
      if (descriptor_ != -1) {
        ::close(descriptor_);
        descriptor_ = -1;
      }
      throw_error(
          "IO_PROJECT_LOCKED",
          "The project is already open or its lock cannot be acquired.");
    }
    struct stat status {};
    if (::fstat(descriptor_, &status) != 0 || !S_ISREG(status.st_mode)) {
      static_cast<void>(::flock(descriptor_, LOCK_UN));
      ::close(descriptor_);
      descriptor_ = -1;
      throw_error(
          "IO_PROJECT_LOCK",
          "The project lock file is not a regular local file.");
    }
#endif
  }

  ProjectLock(const ProjectLock&) = delete;
  ProjectLock& operator=(const ProjectLock&) = delete;

  ~ProjectLock() {
#ifdef _WIN32
    if (handle_ != INVALID_HANDLE_VALUE) {
      CloseHandle(handle_);
    }
#else
    if (descriptor_ != -1) {
      static_cast<void>(::flock(descriptor_, LOCK_UN));
      ::close(descriptor_);
    }
#endif
  }

 private:
#ifdef _WIN32
  HANDLE handle_{INVALID_HANDLE_VALUE};
#else
  int descriptor_{-1};
#endif
};

class OwnedStagingDirectory final {
 public:
  explicit OwnedStagingDirectory(std::filesystem::path path)
      : path_(std::move(path)) {}

  OwnedStagingDirectory(const OwnedStagingDirectory&) = delete;
  OwnedStagingDirectory& operator=(const OwnedStagingDirectory&) = delete;

  ~OwnedStagingDirectory() {
    if (!published_) {
      std::error_code ignored;
      static_cast<void>(std::filesystem::remove_all(path_, ignored));
    }
  }

  void release_after_publish() noexcept { published_ = true; }

 private:
  std::filesystem::path path_;
  bool published_{};
};

class Statement final {
 public:
  Statement(sqlite3* database, std::string_view sql) {
    const int result = sqlite3_prepare_v3(
        database,
        sql.data(),
        static_cast<int>(sql.size()),
        SQLITE_PREPARE_PERSISTENT,
        &statement_,
        nullptr);
    if (result != SQLITE_OK) {
      throw_error("IO_DATABASE_PREPARE", "A project database query is invalid.");
    }
  }

  Statement(const Statement&) = delete;
  Statement& operator=(const Statement&) = delete;

  ~Statement() {
    if (statement_ != nullptr) {
      sqlite3_finalize(statement_);
    }
  }

  void bind(int index, std::string_view value) {
    const int result = sqlite3_bind_text(
        statement_,
        index,
        value.data(),
        static_cast<int>(value.size()),
        SQLITE_TRANSIENT);
    if (result != SQLITE_OK) {
      throw_error("IO_DATABASE_BIND", "A project database value is invalid.");
    }
  }

  void bind(int index, std::int64_t value) {
    if (sqlite3_bind_int64(statement_, index, value) != SQLITE_OK) {
      throw_error("IO_DATABASE_BIND", "A project database value is invalid.");
    }
  }

  void bind(int index, double value) {
    if (sqlite3_bind_double(statement_, index, value) != SQLITE_OK) {
      throw_error("IO_DATABASE_BIND", "A project database value is invalid.");
    }
  }

  void bind_null(int index) {
    if (sqlite3_bind_null(statement_, index) != SQLITE_OK) {
      throw_error("IO_DATABASE_BIND", "A project database value is invalid.");
    }
  }

  [[nodiscard]] bool row() {
    const int result = sqlite3_step(statement_);
    if (result == SQLITE_ROW) {
      return true;
    }
    if (result == SQLITE_DONE) {
      return false;
    }
    throw_error("IO_DATABASE_STEP", "A project database query failed.");
  }

  void done() {
    const int result = sqlite3_step(statement_);
    if (result != SQLITE_DONE) {
      throw_error("IO_DATABASE_STEP", "A project database update failed.");
    }
  }

  void reset() {
    if (sqlite3_reset(statement_) != SQLITE_OK ||
        sqlite3_clear_bindings(statement_) != SQLITE_OK) {
      throw_error(
          "IO_DATABASE_STEP",
          "A project database statement could not be reused.");
    }
  }

  [[nodiscard]] std::int64_t integer(int column) const {
    return sqlite3_column_int64(statement_, column);
  }

  [[nodiscard]] double real(int column) const {
    return sqlite3_column_double(statement_, column);
  }

  [[nodiscard]] std::string text(int column) const {
    const auto* value = sqlite3_column_text(statement_, column);
    const int bytes = sqlite3_column_bytes(statement_, column);
    if (value == nullptr || bytes < 0) {
      throw_error("IO_DATABASE_VALUE", "A project database value is missing.");
    }
    return {
        reinterpret_cast<const char*>(value),
        static_cast<std::size_t>(bytes)};
  }

  [[nodiscard]] bool is_null(int column) const noexcept {
    return sqlite3_column_type(statement_, column) == SQLITE_NULL;
  }

 private:
  sqlite3_stmt* statement_{};
};

class Database final {
 public:
  Database(const std::filesystem::path& path, int flags) {
    const std::string path_text = path_to_utf8(path);
    const int result =
        sqlite3_open_v2(path_text.c_str(), &database_, flags, nullptr);
    if (result != SQLITE_OK) {
      if (database_ != nullptr) {
        sqlite3_close_v2(database_);
        database_ = nullptr;
      }
      throw_error("IO_DATABASE_OPEN", "The project database could not be opened.");
    }
    sqlite3_extended_result_codes(database_, 1);
    sqlite3_busy_timeout(database_, 5'000);
  }

  Database(const Database&) = delete;
  Database& operator=(const Database&) = delete;

  ~Database() {
    if (database_ != nullptr) {
      sqlite3_close_v2(database_);
    }
  }

  [[nodiscard]] sqlite3* get() const noexcept { return database_; }

  void exec(std::string_view sql) const {
    char* message = nullptr;
    const std::string statement(sql);
    const int result =
        sqlite3_exec(database_, statement.c_str(), nullptr, nullptr, &message);
    if (message != nullptr) {
      sqlite3_free(message);
    }
    if (result != SQLITE_OK) {
      throw_error("IO_DATABASE_EXECUTE", "A project database update failed.");
    }
  }

 private:
  sqlite3* database_{};
};

void configure_database(const Database& database) {
  database.exec("PRAGMA foreign_keys = ON;");
  {
    Statement journal_mode(database.get(), "PRAGMA journal_mode = WAL;");
    if (!journal_mode.row() || journal_mode.text(0) != "wal" ||
        journal_mode.row()) {
      throw_error(
          "IO_DATABASE_WAL",
          "The project database could not enter WAL mode.");
    }
  }
  database.exec("PRAGMA synchronous = FULL;");
  database.exec("PRAGMA temp_store = MEMORY;");
  database.exec("PRAGMA trusted_schema = OFF;");
  database.exec("PRAGMA legacy_alter_table = OFF;");
  database.exec("PRAGMA writable_schema = OFF;");
  database.exec("PRAGMA ignore_check_constraints = OFF;");
  database.exec("PRAGMA recursive_triggers = OFF;");
}

void configure_read_only_database(const Database& database) {
  database.exec("PRAGMA foreign_keys = ON;");
  database.exec("PRAGMA temp_store = MEMORY;");
  database.exec("PRAGMA trusted_schema = OFF;");
  database.exec("PRAGMA legacy_alter_table = OFF;");
  database.exec("PRAGMA writable_schema = OFF;");
  database.exec("PRAGMA ignore_check_constraints = OFF;");
  database.exec("PRAGMA recursive_triggers = OFF;");
  database.exec("PRAGMA query_only = ON;");
}

[[nodiscard]] std::optional<std::string_view> expected_table_sql(
    const std::string_view name,
    const int format_version) {
  if (name == "meta") {
    return "CREATE TABLE meta(key TEXT PRIMARY KEY NOT NULL,"
           "value TEXT NOT NULL) STRICT";
  }
  if (name == "objects") {
    return "CREATE TABLE objects(hash TEXT PRIMARY KEY NOT NULL,"
           "byte_size INTEGER NOT NULL CHECK(byte_size > 0),"
           "media_type TEXT NOT NULL) STRICT";
  }
  if (name == "snapshots") {
    if (format_version == kProjectUserVersionV1) {
      return "CREATE TABLE snapshots(id INTEGER PRIMARY KEY,"
             "parent_snapshot_id INTEGER REFERENCES snapshots(id),"
             "created_revision INTEGER NOT NULL UNIQUE,"
             "source_hash TEXT NOT NULL REFERENCES objects(hash),"
             "exposure_ev REAL NOT NULL) STRICT";
    }
    if (format_version == kProjectUserVersionV2) {
      return "CREATE TABLE \"snapshots\"(id INTEGER PRIMARY KEY,"
             "parent_snapshot_id INTEGER REFERENCES \"snapshots\"(id),"
             "created_revision INTEGER NOT NULL UNIQUE,"
             "source_hash TEXT NOT NULL REFERENCES objects(hash),"
             "exposure_ev REAL NOT NULL,"
             "edit_graph_json TEXT NOT NULL "
             "CHECK(length(edit_graph_json) BETWEEN 1 AND 4194304),"
             "edit_graph_sha256 TEXT NOT NULL "
             "CHECK(length(edit_graph_sha256) = 64),"
             "working_color_id TEXT NOT NULL) STRICT";
    }
    return std::nullopt;
  }
  if (name == "history") {
    return "CREATE TABLE history("
           "position INTEGER PRIMARY KEY CHECK(position >= 0),"
           "snapshot_id INTEGER NOT NULL REFERENCES snapshots(id)) STRICT";
  }
  if (name == "transactions") {
    return "CREATE TABLE transactions("
           "revision INTEGER PRIMARY KEY CHECK(revision > 0),"
           "base_revision INTEGER NOT NULL CHECK(base_revision >= 0),"
           "command_id TEXT NOT NULL UNIQUE,"
           "idempotency_key TEXT NOT NULL UNIQUE,"
           "request_fingerprint TEXT NOT NULL,"
           "kind TEXT NOT NULL,"
           "snapshot_id INTEGER NOT NULL REFERENCES snapshots(id)) STRICT";
  }
  if (name == "idempotency") {
    return "CREATE TABLE idempotency("
           "idempotency_key TEXT PRIMARY KEY NOT NULL,"
           "request_fingerprint TEXT NOT NULL,"
           "command_id TEXT NOT NULL,"
           "base_revision INTEGER NOT NULL,"
           "new_revision INTEGER NOT NULL,"
           "snapshot_id INTEGER NOT NULL REFERENCES snapshots(id)) STRICT";
  }
  return std::nullopt;
}

void validate_database_schema(
    const Database& database,
    const int format_version) {
  constexpr std::array<std::pair<std::string_view, std::string_view>, 6>
      expected_indexes{{
          {"sqlite_autoindex_idempotency_1", "idempotency"},
          {"sqlite_autoindex_meta_1", "meta"},
          {"sqlite_autoindex_objects_1", "objects"},
          {"sqlite_autoindex_snapshots_1", "snapshots"},
          {"sqlite_autoindex_transactions_1", "transactions"},
          {"sqlite_autoindex_transactions_2", "transactions"},
      }};
  std::set<std::string> seen_tables;
  std::set<std::string> seen_indexes;
  Statement objects(
      database.get(),
      "SELECT type, name, tbl_name, sql "
      "FROM sqlite_schema ORDER BY type, name;");
  while (objects.row()) {
    const std::string type = objects.text(0);
    const std::string name = objects.text(1);
    const std::string table = objects.text(2);
    if (type == "table") {
      const auto expected_sql =
          expected_table_sql(name, format_version);
      if (!expected_sql.has_value() || table != name ||
          objects.is_null(3) || objects.text(3) != *expected_sql ||
          !seen_tables.insert(name).second) {
        throw_error(
            "IO_PROJECT_SCHEMA",
            "The project database schema is not the exact supported format.");
      }
      continue;
    }
    if (type == "index") {
      const auto expected = std::ranges::find_if(
          expected_indexes,
          [&name, &table](const auto& candidate) {
            return candidate.first == name &&
                   candidate.second == table;
          });
      if (expected == expected_indexes.end() ||
          !objects.is_null(3) ||
          !seen_indexes.insert(name).second) {
        throw_error(
            "IO_PROJECT_SCHEMA",
            "The project database schema is not the exact supported format.");
      }
      continue;
    }
    throw_error(
        "IO_PROJECT_SCHEMA",
        "The project database schema contains an unsupported object.");
  }
  if (seen_tables.size() != 6U ||
      seen_indexes.size() != expected_indexes.size()) {
    throw_error(
        "IO_PROJECT_SCHEMA",
        "The project database schema is incomplete.");
  }

  const std::set<std::string> expected_meta_keys =
      format_version == kProjectUserVersionV2
          ? std::set<std::string>{
                "clean_shutdown",
                "current_revision",
                "current_snapshot_id",
                "document_id",
                "format",
                "history_position",
                "migration_source_document_id",
                "migration_source_fingerprint",
                "migration_source_format"}
          : std::set<std::string>{
                "clean_shutdown",
                "current_revision",
                "current_snapshot_id",
                "document_id",
                "format",
                "history_position"};
  std::set<std::string> actual_meta_keys;
  Statement meta_keys(
      database.get(), "SELECT key FROM meta ORDER BY key;");
  while (meta_keys.row()) {
    if (!actual_meta_keys.insert(meta_keys.text(0)).second) {
      throw_error(
          "IO_PROJECT_SCHEMA",
          "The project metadata schema is ambiguous.");
    }
  }
  if (actual_meta_keys != expected_meta_keys) {
    throw_error(
        "IO_PROJECT_SCHEMA",
        "The project metadata schema is not the exact supported format.");
  }
}

void backup_database(const Database& source, const Database& destination) {
  sqlite3_backup* backup = sqlite3_backup_init(
      destination.get(), "main", source.get(), "main");
  if (backup == nullptr) {
    throw_error(
        "IO_MIGRATION_BACKUP",
        "The source project database could not be backed up.");
  }
  const int step_result = sqlite3_backup_step(backup, -1);
  const int finish_result = sqlite3_backup_finish(backup);
  if (step_result != SQLITE_DONE || finish_result != SQLITE_OK) {
    throw_error(
        "IO_MIGRATION_BACKUP",
        "The source project database backup did not complete.");
  }
}

void rollback_noexcept(const Database& database) noexcept {
  try {
    database.exec("ROLLBACK;");
  } catch (...) {
  }
}

void set_meta(
    const Database& database,
    std::string_view key,
    std::string_view value) {
  Statement statement(
      database.get(),
      "INSERT INTO meta(key, value) VALUES(?1, ?2) "
      "ON CONFLICT(key) DO UPDATE SET value = excluded.value;");
  statement.bind(1, key);
  statement.bind(2, value);
  statement.done();
}

void set_meta_integer(
    const Database& database,
    std::string_view key,
    std::int64_t value) {
  set_meta(database, key, std::to_string(value));
}

[[nodiscard]] std::string get_meta(
    const Database& database,
    std::string_view key) {
  Statement statement(
      database.get(), "SELECT value FROM meta WHERE key = ?1;");
  statement.bind(1, key);
  if (!statement.row()) {
    throw_error("IO_PROJECT_FORMAT", "Required project metadata is missing.");
  }
  const std::string value = statement.text(0);
  if (statement.row()) {
    throw_error("IO_PROJECT_FORMAT", "Project metadata is ambiguous.");
  }
  return value;
}

[[nodiscard]] std::int64_t get_meta_integer(
    const Database& database,
    std::string_view key) {
  const std::string value = get_meta(database, key);
  std::int64_t parsed = 0;
  const auto [end, error] =
      std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (error != std::errc{} || end != value.data() + value.size()) {
    throw_error("IO_PROJECT_FORMAT", "Project metadata has an invalid integer.");
  }
  return parsed;
}

[[nodiscard]] int validate_database_format(
    const Database& database,
    const std::optional<int> required_version = std::nullopt) {
  {
    Statement application_id(database.get(), "PRAGMA application_id;");
    if (!application_id.row() ||
        application_id.integer(0) != kProjectApplicationId ||
        application_id.row()) {
      throw_error(
          "IO_PROJECT_FORMAT",
          "The project application identifier is invalid.");
    }
  }

  int format_version = 0;
  {
    Statement user_version(database.get(), "PRAGMA user_version;");
    if (!user_version.row()) {
      throw_error(
          "IO_PROJECT_VERSION",
          "The project format version is unsupported.");
    }
    const std::int64_t version = user_version.integer(0);
    if (user_version.row() ||
        (version != kProjectUserVersionV1 &&
         version != kProjectUserVersionV2) ||
        (required_version.has_value() &&
         version != *required_version)) {
      throw_error(
          "IO_PROJECT_VERSION",
          "The project format version is unsupported.");
    }
    format_version = static_cast<int>(version);
  }

  validate_database_schema(database, format_version);
  const std::string_view expected_format =
      format_version == kProjectUserVersionV2
          ? kProjectFormatV2
          : kProjectFormatV1;
  if (get_meta(database, "format") != expected_format) {
    throw_error(
        "IO_PROJECT_VERSION",
        "The project format version is unsupported.");
  }
  return format_version;
}

[[nodiscard]] bool has_exact_json_keys(
    const nlohmann::json& value,
    std::initializer_list<std::string_view> required,
    std::initializer_list<std::string_view> optional = {}) {
  if (!value.is_object()) {
    return false;
  }
  for (const std::string_view key : required) {
    if (!value.contains(std::string(key))) {
      return false;
    }
  }
  for (const auto& [key, ignored] : value.items()) {
    static_cast<void>(ignored);
    const bool is_required =
        std::ranges::find(required, std::string_view(key)) != required.end();
    const bool is_optional =
        std::ranges::find(optional, std::string_view(key)) != optional.end();
    if (!is_required && !is_optional) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool is_finite_json_number(const nlohmann::json& value) {
  if (!value.is_number()) {
    return false;
  }
  try {
    const double parsed = value.get<double>();
    return std::isfinite(parsed) &&
           !(parsed == 0.0 && std::signbit(parsed));
  } catch (const nlohmann::json::exception&) {
    return false;
  }
}

[[nodiscard]] bool is_stable_graph_identifier(std::string_view value) {
  if (value.empty() || value.size() > 128U) {
    return false;
  }
  const auto ascii_alphanumeric = [](char character) {
    return (character >= 'a' && character <= 'z') ||
           (character >= 'A' && character <= 'Z') ||
           (character >= '0' && character <= '9');
  };
  if (!ascii_alphanumeric(value.front())) {
    return false;
  }
  return std::ranges::all_of(value, [&](char character) {
    return ascii_alphanumeric(character) || character == '.' ||
           character == '_' || character == ':' || character == '-';
  });
}

[[nodiscard]] bool validate_canonical_edit_graph(
    std::string_view canonical_json,
    std::string_view expected_working_color) {
  if (canonical_json.empty() ||
      canonical_json.size() > kMaximumEditGraphJsonBytes ||
      expected_working_color != kWorkingColorId) {
    return false;
  }
  const auto parsed =
      nps::document::parse_edit_graph_json(canonical_json);
  const auto* graph =
      std::get_if<nps::document::EditGraph>(&parsed);
  if (graph == nullptr ||
      graph->working_color_space() != expected_working_color ||
      nps::document::canonical_edit_graph_json(*graph) != canonical_json) {
    return false;
  }

  nlohmann::json root;
  try {
    root = nlohmann::json::parse(
        canonical_json.begin(), canonical_json.end());
  } catch (const nlohmann::json::exception&) {
    return false;
  }
  if (root.dump() != canonical_json ||
      !has_exact_json_keys(
          root,
          {"schema", "graphId", "workingColorSpace", "sourceNodeId",
           "outputNodeId", "nodes"}) ||
      !root.at("schema").is_string() ||
      root.at("schema").get_ref<const std::string&>() != kEditGraphSchema ||
      !root.at("graphId").is_string() ||
      !is_stable_graph_identifier(
          root.at("graphId").get_ref<const std::string&>()) ||
      !root.at("workingColorSpace").is_string() ||
      root.at("workingColorSpace").get_ref<const std::string&>() !=
          expected_working_color ||
      !root.at("sourceNodeId").is_string() ||
      !root.at("outputNodeId").is_string() ||
      !root.at("nodes").is_array() || root.at("nodes").empty() ||
      root.at("nodes").size() > 4096U) {
    return false;
  }

  struct NodeShape {
    std::string type;
    std::vector<std::string> inputs;
  };
  std::unordered_map<std::string, NodeShape> nodes;
  nodes.reserve(root.at("nodes").size());
  std::string previous_node_id;
  std::size_t source_count = 0U;
  std::size_t output_count = 0U;

  for (const nlohmann::json& node : root.at("nodes")) {
    if (!has_exact_json_keys(
            node,
            {"nodeId", "type", "algorithmVersion", "enabled", "opacity",
             "computeDomain", "inputs", "parameters"},
            {"mask"}) ||
        !node.at("nodeId").is_string() || !node.at("type").is_string() ||
        !node.at("algorithmVersion").is_string() ||
        node.at("algorithmVersion").get_ref<const std::string&>() != "1.0.0" ||
        !node.at("enabled").is_boolean() ||
        !is_finite_json_number(node.at("opacity")) ||
        node.at("opacity").get<double>() < 0.0 ||
        node.at("opacity").get<double>() > 1.0 ||
        !node.at("computeDomain").is_string() ||
        node.at("computeDomain").get_ref<const std::string&>() !=
            "scene-linear" ||
        !node.at("inputs").is_array() ||
        !node.at("parameters").is_object()) {
      return false;
    }

    const std::string node_id =
        node.at("nodeId").get_ref<const std::string&>();
    const std::string type = node.at("type").get_ref<const std::string&>();
    if (!is_stable_graph_identifier(node_id) ||
        (!previous_node_id.empty() && node_id <= previous_node_id) ||
        (type != "source" && type != "adjust.exposure" &&
         type != "adjust.curve.rgb" && type != "output")) {
      return false;
    }
    previous_node_id = node_id;

    std::vector<std::string> inputs;
    inputs.reserve(node.at("inputs").size());
    for (const nlohmann::json& input : node.at("inputs")) {
      if (!input.is_string() ||
          !is_stable_graph_identifier(
              input.get_ref<const std::string&>())) {
        return false;
      }
      inputs.push_back(input.get_ref<const std::string&>());
    }

    if (node.contains("mask")) {
      const nlohmann::json& mask = node.at("mask");
      if ((type == "source" || type == "output") ||
          !has_exact_json_keys(
              mask, {"maskId", "contentHash", "inverted"}) ||
          !mask.at("maskId").is_string() ||
          !is_stable_graph_identifier(
              mask.at("maskId").get_ref<const std::string&>()) ||
          !mask.at("contentHash").is_string() ||
          !is_lower_hex(
              mask.at("contentHash").get_ref<const std::string&>()) ||
          !mask.at("inverted").is_boolean()) {
        return false;
      }
    }

    const nlohmann::json& parameters = node.at("parameters");
    if (type == "source") {
      ++source_count;
      if (!node.at("enabled").get<bool>() ||
          node.at("opacity").get<double>() != 1.0 ||
          !inputs.empty() || !parameters.empty()) {
        return false;
      }
    } else if (type == "output") {
      ++output_count;
      if (!node.at("enabled").get<bool>() ||
          node.at("opacity").get<double>() != 1.0 ||
          inputs.size() != 1U || !parameters.empty()) {
        return false;
      }
    } else if (type == "adjust.exposure") {
      if (inputs.size() != 1U ||
          !has_exact_json_keys(parameters, {"ev"}) ||
          !is_finite_json_number(parameters.at("ev")) ||
          parameters.at("ev").get<double>() < -10.0 ||
          parameters.at("ev").get<double>() > 10.0) {
        return false;
      }
    } else {
      if (inputs.size() != 1U ||
          !has_exact_json_keys(parameters, {"points"}) ||
          !parameters.at("points").is_array() ||
          parameters.at("points").size() < 2U ||
          parameters.at("points").size() > 256U) {
        return false;
      }
      double previous_x = -1.0;
      std::size_t point_index = 0U;
      for (const nlohmann::json& point : parameters.at("points")) {
        if (!has_exact_json_keys(point, {"x", "y"}) ||
            !is_finite_json_number(point.at("x")) ||
            !is_finite_json_number(point.at("y"))) {
          return false;
        }
        const double x = point.at("x").get<double>();
        const double y = point.at("y").get<double>();
        if (x < 0.0 || x > 1.0 || y < 0.0 || y > 1.0 ||
            x <= previous_x ||
            (point_index == 0U && x != 0.0)) {
          return false;
        }
        previous_x = x;
        ++point_index;
      }
      if (previous_x != 1.0) {
        return false;
      }
    }

    if (!nodes.emplace(
             node_id,
             NodeShape{.type = type, .inputs = std::move(inputs)})
             .second) {
      return false;
    }
  }

  const std::string source_id =
      root.at("sourceNodeId").get_ref<const std::string&>();
  const std::string output_id =
      root.at("outputNodeId").get_ref<const std::string&>();
  const auto source = nodes.find(source_id);
  const auto output = nodes.find(output_id);
  if (source_count != 1U || output_count != 1U ||
      source == nodes.end() || source->second.type != "source" ||
      output == nodes.end() || output->second.type != "output") {
    return false;
  }

  std::unordered_map<std::string, std::size_t> indegree;
  std::unordered_map<std::string, std::vector<std::string>> dependents;
  indegree.reserve(nodes.size());
  dependents.reserve(nodes.size());
  for (const auto& [node_id, shape] : nodes) {
    indegree[node_id] = shape.inputs.size();
    for (const std::string& input : shape.inputs) {
      if (!nodes.contains(input) || input == node_id) {
        return false;
      }
      dependents[input].push_back(node_id);
    }
  }

  std::vector<std::string> ready;
  ready.reserve(nodes.size());
  for (const auto& [node_id, degree] : indegree) {
    if (degree == 0U) {
      ready.push_back(node_id);
    }
  }
  std::size_t visited = 0U;
  while (!ready.empty()) {
    const std::string node_id = std::move(ready.back());
    ready.pop_back();
    ++visited;
    for (const std::string& dependent : dependents[node_id]) {
      std::size_t& degree = indegree[dependent];
      if (--degree == 0U) {
        ready.push_back(dependent);
      }
    }
  }
  if (visited != nodes.size()) {
    return false;
  }

  std::set<std::string> reachable;
  std::vector<std::string> pending{output_id};
  while (!pending.empty()) {
    std::string node_id = std::move(pending.back());
    pending.pop_back();
    if (!reachable.insert(node_id).second) {
      continue;
    }
    for (const std::string& input : nodes.at(node_id).inputs) {
      pending.push_back(input);
    }
  }
  return reachable.size() == nodes.size() && reachable.contains(source_id);
}

[[nodiscard]] nlohmann::json source_node_json() {
  return {
      {"nodeId", "source"},
      {"type", "source"},
      {"algorithmVersion", "1.0.0"},
      {"enabled", true},
      {"opacity", 1.0},
      {"computeDomain", "scene-linear"},
      {"inputs", nlohmann::json::array()},
      {"parameters", nlohmann::json::object()}};
}

[[nodiscard]] nlohmann::json output_node_json(std::string input) {
  return {
      {"nodeId", "output"},
      {"type", "output"},
      {"algorithmVersion", "1.0.0"},
      {"enabled", true},
      {"opacity", 1.0},
      {"computeDomain", "scene-linear"},
      {"inputs", nlohmann::json::array({std::move(input)})},
      {"parameters", nlohmann::json::object()}};
}

[[nodiscard]] nlohmann::json exposure_node_json(
    std::string node_id,
    std::string input,
    double exposure_delta_ev) {
  if (exposure_delta_ev == 0.0) {
    exposure_delta_ev = 0.0;
  }
  return {
      {"nodeId", std::move(node_id)},
      {"type", "adjust.exposure"},
      {"algorithmVersion", "1.0.0"},
      {"enabled", true},
      {"opacity", 1.0},
      {"computeDomain", "scene-linear"},
      {"inputs", nlohmann::json::array({std::move(input)})},
      {"parameters", {{"ev", exposure_delta_ev}}}};
}

[[nodiscard]] std::string canonical_graph_json(
    std::string graph_id,
    std::vector<nlohmann::json> exposure_nodes) {
  std::string output_input = "source";
  if (!exposure_nodes.empty()) {
    output_input = exposure_nodes.back().at("nodeId").get<std::string>();
  }

  nlohmann::json nodes = nlohmann::json::array();
  for (nlohmann::json& node : exposure_nodes) {
    nodes.push_back(std::move(node));
  }
  nodes.push_back(output_node_json(std::move(output_input)));
  nodes.push_back(source_node_json());

  return nlohmann::json{
      {"schema", std::string(kEditGraphSchema)},
      {"graphId", std::move(graph_id)},
      {"workingColorSpace", std::string(kWorkingColorId)},
      {"sourceNodeId", "source"},
      {"outputNodeId", "output"},
      {"nodes", std::move(nodes)}}
      .dump();
}

[[nodiscard]] std::string append_exposure_to_graph(
    std::string_view canonical_json,
    std::string graph_id,
    std::string node_id,
    double exposure_delta_ev) {
  nlohmann::json root =
      nlohmann::json::parse(canonical_json.begin(), canonical_json.end());
  nlohmann::json& nodes = root.at("nodes");
  const std::string& output_node_id =
      root.at("outputNodeId").get_ref<const std::string&>();
  auto output = std::ranges::find_if(
      nodes,
      [&output_node_id](const nlohmann::json& node) {
        return node.at("nodeId").get_ref<const std::string&>() ==
               output_node_id;
      });
  if (output == nodes.end()) {
    throw_error("IO_PROJECT_FORMAT", "The snapshot edit graph is invalid.");
  }
  const std::string requested_node_id = node_id;
  std::size_t suffix = 0U;
  const auto node_id_exists = [&nodes](std::string_view candidate) {
    return std::ranges::any_of(
        nodes,
        [candidate](const nlohmann::json& node) {
          return node.at("nodeId").get_ref<const std::string&>() ==
                 candidate;
        });
  };
  while (node_id_exists(node_id)) {
    ++suffix;
    node_id =
        requested_node_id + "-auto-" + std::to_string(suffix);
  }
  const std::string previous_input =
      output->at("inputs").at(0).get<std::string>();
  output->at("inputs") = nlohmann::json::array({node_id});
  nodes.push_back(exposure_node_json(
      std::move(node_id), previous_input, exposure_delta_ev));
  std::vector<nlohmann::json> sorted_nodes;
  sorted_nodes.reserve(nodes.size());
  for (nlohmann::json& node : nodes) {
    sorted_nodes.push_back(std::move(node));
  }
  std::ranges::sort(
      sorted_nodes,
      [](const nlohmann::json& left, const nlohmann::json& right) {
        return left.at("nodeId").get_ref<const std::string&>() <
               right.at("nodeId").get_ref<const std::string&>();
      });
  root.at("nodes") = std::move(sorted_nodes);
  root.at("graphId") = std::move(graph_id);
  return root.dump();
}

[[nodiscard]] Snapshot read_snapshot(
    const Database& database,
    std::int64_t snapshot_id,
    int format_version) {
  const std::string query =
      format_version == kProjectUserVersionV2
          ? "SELECT id, created_revision, source_hash, exposure_ev, "
            "edit_graph_json, edit_graph_sha256, working_color_id "
            "FROM snapshots WHERE id = ?1;"
          : "SELECT id, created_revision, source_hash, exposure_ev "
            "FROM snapshots WHERE id = ?1;";
  Statement statement(
      database.get(),
      query);
  statement.bind(1, snapshot_id);
  if (!statement.row()) {
    throw_error("IO_PROJECT_FORMAT", "The current snapshot is missing.");
  }
  Snapshot snapshot{
      .id = statement.integer(0),
      .created_revision = statement.integer(1),
      .source_hash = statement.text(2),
      .exposure_ev = statement.real(3),
      .edit_graph_json = {},
      .edit_graph_sha256 = {},
      .working_color_id = {}};
  if (format_version == kProjectUserVersionV2) {
    snapshot.edit_graph_json = statement.text(4);
    snapshot.edit_graph_sha256 = statement.text(5);
    snapshot.working_color_id = statement.text(6);
  }
  if (statement.row()) {
    throw_error("IO_PROJECT_FORMAT", "The current snapshot is ambiguous.");
  }
  if (!is_lower_hex(snapshot.source_hash) ||
      !std::isfinite(snapshot.exposure_ev)) {
    throw_error("IO_PROJECT_FORMAT", "The current snapshot is invalid.");
  }
  if (format_version == kProjectUserVersionV2 &&
      (!is_lower_hex(snapshot.edit_graph_sha256) ||
       sha256(snapshot.edit_graph_json) != snapshot.edit_graph_sha256 ||
       !validate_canonical_edit_graph(
           snapshot.edit_graph_json, snapshot.working_color_id))) {
    throw_error(
        "IO_PROJECT_FORMAT",
        "The current snapshot edit graph is invalid.");
  }
  return snapshot;
}

[[nodiscard]] std::int64_t history_snapshot(
    const Database& database,
    std::int64_t position) {
  Statement statement(
      database.get(),
      "SELECT snapshot_id FROM history WHERE position = ?1;");
  statement.bind(1, position);
  if (!statement.row()) {
    throw_error("CMD_HISTORY_BOUNDARY", "There is no history entry to select.");
  }
  const std::int64_t snapshot_id = statement.integer(0);
  if (statement.row()) {
    throw_error("IO_PROJECT_FORMAT", "The project history is ambiguous.");
  }
  return snapshot_id;
}

[[nodiscard]] std::int64_t maximum_history_position(
    const Database& database) {
  Statement statement(database.get(), "SELECT MAX(position) FROM history;");
  if (!statement.row()) {
    throw_error("IO_PROJECT_FORMAT", "The project history is missing.");
  }
  return statement.integer(0);
}

[[nodiscard]] std::string mutation_name(StoreMutation mutation) {
  switch (mutation) {
    case StoreMutation::adjust_exposure:
      return "adjust.exposure";
    case StoreMutation::replace_graph:
      return "graph.replace";
    case StoreMutation::undo:
      return "history.undo";
    case StoreMutation::redo:
      return "history.redo";
  }
  throw_error("CMD_UNSUPPORTED_TYPE", "The command kind is not supported.");
}

void validate_project_root(const std::filesystem::path& project_root) {
  if (project_root.empty() || project_root.filename().empty() ||
      project_root.extension() != ".npsproj") {
    throw_error(
        "IO_PROJECT_PATH",
        "An M0 project must use a non-empty .npsproj directory name.");
  }
}

void check_database_integrity(const Database& database) {
  Statement statement(database.get(), "PRAGMA integrity_check;");
  bool found = false;
  while (statement.row()) {
    found = true;
    if (statement.text(0) != "ok") {
      throw_error("IO_DATABASE_CORRUPT", "The project database failed integrity checking.");
    }
  }
  if (!found) {
    throw_error("IO_DATABASE_CORRUPT", "The project database returned no integrity result.");
  }

  Statement foreign_keys(database.get(), "PRAGMA foreign_key_check;");
  if (foreign_keys.row()) {
    throw_error(
        "IO_DATABASE_CORRUPT",
        "The project database failed foreign-key checking.");
  }
}

[[nodiscard]] std::string migration_source_fingerprint(
    const Database& database,
    const bool project_is_v2 = false) {
  nlohmann::json facts;
  facts["meta"] = nlohmann::json::array();
  if (project_is_v2) {
    for (const std::string_view key :
         {"current_revision",
          "current_snapshot_id",
          "document_id"}) {
      facts["meta"].push_back(nlohmann::json::array(
          {key, get_meta(database, key)}));
    }
    facts["meta"].push_back(
        nlohmann::json::array({"format", kProjectFormatV1}));
    facts["meta"].push_back(nlohmann::json::array(
        {"history_position", get_meta(database, "history_position")}));
  } else {
    Statement rows(
        database.get(),
        "SELECT key, value FROM meta WHERE key <> 'clean_shutdown' "
        "ORDER BY key;");
    while (rows.row()) {
      facts["meta"].push_back(
          nlohmann::json::array({rows.text(0), rows.text(1)}));
    }
  }
  facts["objects"] = nlohmann::json::array();
  {
    Statement rows(
        database.get(),
        "SELECT hash, byte_size, media_type FROM objects ORDER BY hash;");
    while (rows.row()) {
      facts["objects"].push_back(nlohmann::json::array(
          {rows.text(0), rows.integer(1), rows.text(2)}));
    }
  }
  facts["snapshots"] = nlohmann::json::array();
  {
    Statement rows(
        database.get(),
        "SELECT id, parent_snapshot_id, created_revision, source_hash, "
        "exposure_ev FROM snapshots ORDER BY id;");
    while (rows.row()) {
      nlohmann::json parent = nullptr;
      if (!rows.is_null(1)) {
        parent = rows.integer(1);
      }
      facts["snapshots"].push_back(nlohmann::json::array(
          {rows.integer(0),
           std::move(parent),
           rows.integer(2),
           rows.text(3),
           rows.real(4)}));
    }
  }
  facts["history"] = nlohmann::json::array();
  {
    Statement rows(
        database.get(),
        "SELECT position, snapshot_id FROM history ORDER BY position;");
    while (rows.row()) {
      facts["history"].push_back(
          nlohmann::json::array({rows.integer(0), rows.integer(1)}));
    }
  }
  facts["transactions"] = nlohmann::json::array();
  {
    Statement rows(
        database.get(),
        "SELECT revision, base_revision, command_id, idempotency_key, "
        "request_fingerprint, kind, snapshot_id "
        "FROM transactions ORDER BY revision;");
    while (rows.row()) {
      facts["transactions"].push_back(nlohmann::json::array(
          {rows.integer(0),
           rows.integer(1),
           rows.text(2),
           rows.text(3),
           rows.text(4),
           rows.text(5),
           rows.integer(6)}));
    }
  }
  facts["idempotency"] = nlohmann::json::array();
  {
    Statement rows(
        database.get(),
        "SELECT idempotency_key, request_fingerprint, command_id, "
        "base_revision, new_revision, snapshot_id "
        "FROM idempotency ORDER BY idempotency_key;");
    while (rows.row()) {
      facts["idempotency"].push_back(nlohmann::json::array(
          {rows.text(0),
           rows.text(1),
           rows.text(2),
           rows.integer(3),
           rows.integer(4),
           rows.integer(5)}));
    }
  }
  return sha256(facts.dump());
}

void validate_v1_migration_semantics(const Database& database) {
  struct LegacySnapshotShape {
    std::optional<std::int64_t> parent_id;
    std::int64_t created_revision{};
    double exposure_ev{};
  };
  std::unordered_map<std::int64_t, LegacySnapshotShape> snapshots;
  {
    Statement rows(
        database.get(),
        "SELECT id, parent_snapshot_id, created_revision, exposure_ev "
        "FROM snapshots ORDER BY id;");
    while (rows.row()) {
      const std::int64_t snapshot_id = rows.integer(0);
      std::optional<std::int64_t> parent_id;
      if (!rows.is_null(1)) {
        parent_id = rows.integer(1);
      }
      const LegacySnapshotShape shape{
          .parent_id = parent_id,
          .created_revision = rows.integer(2),
          .exposure_ev = rows.real(3)};
      if (snapshot_id <= 0 || shape.created_revision < 0 ||
          !std::isfinite(shape.exposure_ev) ||
          !snapshots.emplace(snapshot_id, shape).second) {
        throw_error(
            "IO_PROJECT_FORMAT",
            "The legacy snapshot structure is invalid.");
      }
    }
  }
  const auto initial = snapshots.find(1);
  if (snapshots.empty() || initial == snapshots.end() ||
      initial->second.parent_id.has_value() ||
      initial->second.created_revision != 0 ||
      initial->second.exposure_ev != 0.0) {
    throw_error(
        "IO_PROJECT_FORMAT",
        "The legacy initial snapshot is invalid.");
  }

  std::size_t root_count = 0U;
  for (const auto& [snapshot_id, shape] : snapshots) {
    if (!shape.parent_id.has_value()) {
      ++root_count;
      if (snapshot_id != 1) {
        throw_error(
            "IO_PROJECT_FORMAT",
            "The legacy project has an unexpected snapshot root.");
      }
      continue;
    }
    const auto parent = snapshots.find(*shape.parent_id);
    if (parent == snapshots.end() ||
        parent->second.created_revision >= shape.created_revision) {
      throw_error(
          "IO_PROJECT_FORMAT",
          "The legacy snapshot ancestry is invalid.");
    }
    const double delta = shape.exposure_ev - parent->second.exposure_ev;
    if (!std::isfinite(delta) || delta < -10.0 || delta > 10.0) {
      throw_error(
          "IO_PROJECT_FORMAT",
          "A legacy snapshot is inconsistent with an exposure command.");
    }
  }
  if (root_count != 1U) {
    throw_error(
        "IO_PROJECT_FORMAT",
        "The legacy project must contain exactly one snapshot root.");
  }

  {
    Statement invalid_transactions(
        database.get(),
        "SELECT 1 FROM transactions "
        "WHERE base_revision <> revision - 1 "
        "OR kind NOT IN ('adjust.exposure', 'history.undo', 'history.redo') "
        "LIMIT 1;");
    if (invalid_transactions.row()) {
      throw_error(
          "IO_PROJECT_FORMAT",
          "The legacy transaction sequence is invalid.");
    }
  }
  {
    Statement invalid_snapshot_transactions(
        database.get(),
        "SELECT 1 FROM snapshots AS s "
        "LEFT JOIN transactions AS t ON t.revision = s.created_revision "
        "WHERE s.created_revision > 0 "
        "AND (t.revision IS NULL OR t.kind <> 'adjust.exposure' "
        "OR t.snapshot_id <> s.id) LIMIT 1;");
    if (invalid_snapshot_transactions.row()) {
      throw_error(
          "IO_PROJECT_FORMAT",
          "A legacy snapshot is missing its creation transaction.");
    }
  }
  {
    Statement invalid_idempotency(
        database.get(),
        "SELECT 1 FROM idempotency AS i "
        "LEFT JOIN transactions AS t "
        "ON t.idempotency_key = i.idempotency_key "
        "WHERE t.revision IS NULL "
        "OR t.request_fingerprint <> i.request_fingerprint "
        "OR t.command_id <> i.command_id "
        "OR t.base_revision <> i.base_revision "
        "OR t.revision <> i.new_revision "
        "OR t.snapshot_id <> i.snapshot_id LIMIT 1;");
    if (invalid_idempotency.row()) {
      throw_error(
          "IO_PROJECT_FORMAT",
          "The legacy idempotency log is invalid.");
    }
    Statement counts(
        database.get(),
        "SELECT (SELECT COUNT(*) FROM transactions), "
        "(SELECT COUNT(*) FROM idempotency);");
    if (!counts.row() || counts.integer(0) != counts.integer(1) ||
        counts.row()) {
      throw_error(
          "IO_PROJECT_FORMAT",
          "The legacy transaction and idempotency logs disagree.");
    }
  }
}

[[nodiscard]] std::string fixed_width_node_index(std::size_t index) {
  std::string value = std::to_string(index);
  if (value.size() > 10U) {
    throw_error(
        "IO_MIGRATION_GRAPH_LIMIT",
        "A legacy exposure cannot be represented by the v2 graph limit.");
  }
  return std::string(10U - value.size(), '0') + value;
}

[[nodiscard]] std::string migrated_graph_for_exposure(
    std::int64_t snapshot_id,
    double exposure_ev) {
  if (!std::isfinite(exposure_ev)) {
    throw_error(
        "IO_PROJECT_FORMAT",
        "A legacy snapshot exposure is invalid.");
  }

  std::vector<nlohmann::json> exposure_nodes;
  std::string input = "source";
  double remaining = exposure_ev == 0.0 ? 0.0 : exposure_ev;
  for (std::size_t index = 1U; remaining != 0.0; ++index) {
    if (index > 4094U) {
      throw_error(
          "IO_MIGRATION_GRAPH_LIMIT",
          "A legacy exposure cannot be represented by the v2 graph limit.");
    }
    double delta = remaining;
    if (delta > 10.0) {
      delta = 10.0;
    } else if (delta < -10.0) {
      delta = -10.0;
    }
    const std::string node_id =
        "adjust-exposure-" + fixed_width_node_index(index);
    exposure_nodes.push_back(
        exposure_node_json(node_id, input, delta));
    input = node_id;
    const double next = remaining - delta;
    if (next == remaining) {
      throw_error(
          "IO_MIGRATION_GRAPH_LIMIT",
          "A legacy exposure cannot be represented by the v2 graph limit.");
    }
    remaining = next == 0.0 ? 0.0 : next;
  }
  const std::string graph = canonical_graph_json(
      "graph-migrated-snapshot-" + std::to_string(snapshot_id),
      std::move(exposure_nodes));
  if (!validate_canonical_edit_graph(graph, kWorkingColorId)) {
    throw_error(
        "IO_MIGRATION_GRAPH",
        "A migrated snapshot edit graph failed validation.");
  }
  return graph;
}

void transform_database_v1_to_v2(
    Database& database,
    std::string_view source_fingerprint) {
  database.exec("PRAGMA foreign_keys = OFF;");
  database.exec("BEGIN EXCLUSIVE;");
  try {
    database.exec(
        "CREATE TABLE snapshots_v2("
        "id INTEGER PRIMARY KEY,"
        "parent_snapshot_id INTEGER REFERENCES snapshots_v2(id),"
        "created_revision INTEGER NOT NULL UNIQUE,"
        "source_hash TEXT NOT NULL REFERENCES objects(hash),"
        "exposure_ev REAL NOT NULL,"
        "edit_graph_json TEXT NOT NULL "
        "CHECK(length(edit_graph_json) BETWEEN 1 AND 4194304),"
        "edit_graph_sha256 TEXT NOT NULL CHECK(length(edit_graph_sha256) = 64),"
        "working_color_id TEXT NOT NULL"
        ") STRICT;");

    {
      Statement legacy(
          database.get(),
          "SELECT id, parent_snapshot_id, created_revision, source_hash, "
          "exposure_ev FROM snapshots ORDER BY id;");
      Statement migrated(
          database.get(),
          "INSERT INTO snapshots_v2("
          "id, parent_snapshot_id, created_revision, source_hash, exposure_ev, "
          "edit_graph_json, edit_graph_sha256, working_color_id"
          ") VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8);");
      while (legacy.row()) {
        const std::int64_t snapshot_id = legacy.integer(0);
        const double exposure_ev = legacy.real(4);
        const std::string graph =
            migrated_graph_for_exposure(snapshot_id, exposure_ev);
        migrated.bind(1, snapshot_id);
        if (legacy.is_null(1)) {
          migrated.bind_null(2);
        } else {
          migrated.bind(2, legacy.integer(1));
        }
        migrated.bind(3, legacy.integer(2));
        migrated.bind(4, legacy.text(3));
        migrated.bind(5, exposure_ev);
        migrated.bind(6, graph);
        migrated.bind(7, sha256(graph));
        migrated.bind(8, kWorkingColorId);
        migrated.done();
        migrated.reset();
      }
    }
    database.exec("DROP TABLE snapshots;");
    database.exec("ALTER TABLE snapshots_v2 RENAME TO snapshots;");

    set_meta(database, "format", kProjectFormatV2);
    set_meta(database, "migration_source_format", kProjectFormatV1);
    set_meta(
        database, "migration_source_document_id",
        get_meta(database, "document_id"));
    set_meta(database, "migration_source_fingerprint", source_fingerprint);
    set_meta_integer(database, "clean_shutdown", 1);
    database.exec(
        "PRAGMA user_version = " +
        std::to_string(kProjectUserVersionV2) + ";");
    if (migration_source_fingerprint(database, true) !=
        source_fingerprint) {
      throw_error(
          "IO_MIGRATION_VERIFY",
          "The migrated project facts do not match the validated source.");
    }
    database.exec("COMMIT;");
  } catch (...) {
    rollback_noexcept(database);
    throw;
  }
  database.exec("PRAGMA foreign_keys = ON;");
  check_database_integrity(database);
  static_cast<void>(validate_database_format(
      database, kProjectUserVersionV2));
}

}  // namespace

struct ProjectStore::Impl {
  Impl(
      std::filesystem::path root_value,
      std::unique_ptr<ProjectLock> lock_value,
      std::unique_ptr<Database> database_value,
      bool recovered_value,
      int format_version_value,
      bool read_only_value = false)
      : root(std::move(root_value)),
        lock(std::move(lock_value)),
        database(std::move(database_value)),
        recovered_unclean_shutdown(recovered_value),
        format_version(format_version_value),
        read_only(read_only_value) {}

  std::filesystem::path root;
  std::unique_ptr<ProjectLock> lock;
  std::unique_ptr<Database> database;
  bool recovered_unclean_shutdown{};
  int format_version{kProjectUserVersionV1};
  bool read_only{};
};

ProjectStoreError::ProjectStoreError(std::string code, std::string message)
    : std::runtime_error(std::move(message)), code_(std::move(code)) {}

const std::string& ProjectStoreError::code() const noexcept { return code_; }

ProjectStore::ProjectStore(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

ProjectStore ProjectStore::create(
    const std::filesystem::path& project_root,
    std::span<const std::uint8_t> source_bytes,
    FaultPoint fault_point) {
  validate_project_root(project_root);
  if (source_bytes.empty()) {
    throw_error("IO_SOURCE_EMPTY", "The source image cannot be empty.");
  }

  const std::filesystem::path published_root =
      resolve_project_root(
          project_root,
          "The destination path could not be resolved.");
  if (path_entry_exists_no_follow(
          published_root,
          "IO_PROJECT_STAT",
          "The destination project could not be inspected.")) {
    throw_error("IO_PROJECT_EXISTS", "The destination project already exists.");
  }

  std::error_code error;
  const std::filesystem::path parent = published_root.parent_path();
  require_real_directory(parent);
  const std::filesystem::path staging_root =
      parent / random_identifier(".nps-creating-");
  const bool staging_created =
      std::filesystem::create_directory(staging_root, error);
  if (error || !staging_created) {
    throw_error(
        "IO_PROJECT_CREATE",
        "The private project staging directory could not be reserved.");
  }
  OwnedStagingDirectory staging_cleanup(staging_root);
  std::filesystem::create_directories(
      staging_root / "objects" / "sha256", error);
  if (error) {
    throw_error(
        "IO_PROJECT_CREATE",
        "The private project staging directory could not be created.");
  }
  for (const std::string_view child :
       {"previews", "recovery", "manifests"}) {
    std::filesystem::create_directories(staging_root / child, error);
    if (error) {
      throw_error("IO_PROJECT_CREATE", "A project directory could not be created.");
    }
  }
  {
    std::ofstream marker(
        staging_root / ".nps-creating",
        std::ios::binary | std::ios::trunc);
    marker << "nps.create/v1\n";
    marker.flush();
    if (!marker) {
      throw_error(
          "IO_PROJECT_CREATE",
          "The project staging marker could not be written.");
    }
  }

  const std::string source_hash =
      persist_object(staging_root, source_bytes);
  inject_crash_if_requested(fault_point, FaultPoint::after_object_persisted);

  auto database = std::make_unique<Database>(
      staging_root / "project.db",
      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_EXRESCODE |
          SQLITE_OPEN_NOFOLLOW);
  configure_database(*database);
  Database& project_database = *database;
  project_database.exec("BEGIN IMMEDIATE;");
  try {
    project_database.exec(
        "CREATE TABLE meta("
        "key TEXT PRIMARY KEY NOT NULL,"
        "value TEXT NOT NULL"
        ") STRICT;"
        "CREATE TABLE objects("
        "hash TEXT PRIMARY KEY NOT NULL,"
        "byte_size INTEGER NOT NULL CHECK(byte_size > 0),"
        "media_type TEXT NOT NULL"
        ") STRICT;"
        "CREATE TABLE snapshots("
        "id INTEGER PRIMARY KEY,"
        "parent_snapshot_id INTEGER REFERENCES snapshots(id),"
        "created_revision INTEGER NOT NULL UNIQUE,"
        "source_hash TEXT NOT NULL REFERENCES objects(hash),"
        "exposure_ev REAL NOT NULL"
        ") STRICT;"
        "CREATE TABLE history("
        "position INTEGER PRIMARY KEY CHECK(position >= 0),"
        "snapshot_id INTEGER NOT NULL REFERENCES snapshots(id)"
        ") STRICT;"
        "CREATE TABLE transactions("
        "revision INTEGER PRIMARY KEY CHECK(revision > 0),"
        "base_revision INTEGER NOT NULL CHECK(base_revision >= 0),"
        "command_id TEXT NOT NULL UNIQUE,"
        "idempotency_key TEXT NOT NULL UNIQUE,"
        "request_fingerprint TEXT NOT NULL,"
        "kind TEXT NOT NULL,"
        "snapshot_id INTEGER NOT NULL REFERENCES snapshots(id)"
        ") STRICT;"
        "CREATE TABLE idempotency("
        "idempotency_key TEXT PRIMARY KEY NOT NULL,"
        "request_fingerprint TEXT NOT NULL,"
        "command_id TEXT NOT NULL,"
        "base_revision INTEGER NOT NULL,"
        "new_revision INTEGER NOT NULL,"
        "snapshot_id INTEGER NOT NULL REFERENCES snapshots(id)"
        ") STRICT;");

    set_meta(project_database, "format", kProjectFormatV1);
    set_meta(
        project_database,
        "document_id",
        random_identifier("doc-"));
    set_meta_integer(project_database, "current_revision", 0);
    set_meta_integer(project_database, "current_snapshot_id", 1);
    set_meta_integer(project_database, "history_position", 0);
    set_meta_integer(project_database, "clean_shutdown", 1);

    Statement object_insert(
        project_database.get(),
        "INSERT INTO objects(hash, byte_size, media_type) "
        "VALUES(?1, ?2, 'image/x-portable-pixmap; depth=16');");
    object_insert.bind(1, source_hash);
    if (source_bytes.size() >
        static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())) {
      throw_error("IO_OBJECT_SIZE", "The source object is too large.");
    }
    object_insert.bind(2, static_cast<std::int64_t>(source_bytes.size()));
    object_insert.done();

    Statement snapshot_insert(
        project_database.get(),
        "INSERT INTO snapshots("
        "id, parent_snapshot_id, created_revision, source_hash, exposure_ev"
        ") VALUES(1, NULL, 0, ?1, 0.0);");
    snapshot_insert.bind(1, source_hash);
    snapshot_insert.done();

    project_database.exec(
        "INSERT INTO history(position, snapshot_id) VALUES(0, 1);");
    project_database.exec(
        "PRAGMA application_id = " +
        std::to_string(kProjectApplicationId) + ";");
    project_database.exec(
        "PRAGMA user_version = " +
        std::to_string(kProjectUserVersionV1) + ";");
    project_database.exec("COMMIT;");
  } catch (...) {
    rollback_noexcept(project_database);
    throw;
  }

  static_cast<void>(validate_database_format(
      project_database, kProjectUserVersionV1));
  check_database_integrity(project_database);
  try {
    project_database.exec("PRAGMA wal_checkpoint(TRUNCATE);");
  } catch (...) {
  }
  database.reset();
  std::filesystem::rename(staging_root, published_root, error);
  if (error) {
    if (path_entry_exists_no_follow(
            published_root,
            "IO_PROJECT_STAT",
            "The destination project could not be inspected after publication.")) {
      throw_error(
          "IO_PROJECT_EXISTS",
          "Another process published the destination project first.");
    }
    throw_error(
        "IO_PROJECT_PUBLISH",
        "The completed project could not be published atomically.");
  }
  staging_cleanup.release_after_publish();
  inject_crash_if_requested(fault_point, FaultPoint::after_database_committed);
  return ProjectStore::open(published_root);
}

ProjectStore ProjectStore::open(
    const std::filesystem::path& project_root) {
  validate_project_root(project_root);
  const std::filesystem::path resolved_root =
      resolve_project_root(
          project_root,
          "The project path could not be resolved.");
  std::error_code error;
  const auto root_status = std::filesystem::symlink_status(resolved_root, error);
  if (error || !std::filesystem::is_directory(root_status) ||
      is_link_or_reparse_point(resolved_root, root_status)) {
    throw_error("IO_PROJECT_OPEN", "The project directory is unavailable.");
  }
  const auto database_status =
      std::filesystem::symlink_status(resolved_root / "project.db", error);
  if (error || !std::filesystem::is_regular_file(database_status) ||
      is_link_or_reparse_point(resolved_root / "project.db", database_status)) {
    throw_error("IO_DATABASE_OPEN", "The project database is unavailable.");
  }

  auto project_lock =
      std::make_unique<ProjectLock>(resolved_root / "project.lock");
  auto database = std::make_unique<Database>(
      resolved_root / "project.db",
      SQLITE_OPEN_READWRITE | SQLITE_OPEN_EXRESCODE |
          SQLITE_OPEN_NOFOLLOW);
  configure_database(*database);

  const int format_version = validate_database_format(*database);

  const std::int64_t clean_state =
      get_meta_integer(*database, "clean_shutdown");
  if (clean_state != 0 && clean_state != 1) {
    throw_error(
        "IO_PROJECT_FORMAT",
        "The clean-shutdown marker is invalid.");
  }
  const bool recovered = clean_state == 0;
  if (recovered) {
    check_database_integrity(*database);
  }

  auto implementation = std::make_unique<Impl>(
      resolved_root,
      std::move(project_lock),
      std::move(database),
      recovered,
      format_version);
  ProjectStore project(std::move(implementation));
  static_cast<void>(project.verify_integrity());

  Database& project_database = *project.impl_->database;
  project_database.exec("BEGIN IMMEDIATE;");
  try {
    set_meta_integer(project_database, "clean_shutdown", 0);
    project_database.exec("COMMIT;");
  } catch (...) {
    rollback_noexcept(project_database);
    throw;
  }

  const bool is_v2 = format_version == kProjectUserVersionV2;
  const std::filesystem::path publish_marker =
      resolved_root / (is_v2 ? ".nps-migrating" : ".nps-creating");
  const std::string expected_marker =
      is_v2 ? "nps.migrate/v1-to-v2\n" : "nps.create/v1\n";
  const auto marker_status =
      std::filesystem::symlink_status(publish_marker, error);
  if (!error && std::filesystem::exists(marker_status)) {
    if (!std::filesystem::is_regular_file(marker_status) ||
        is_link_or_reparse_point(publish_marker, marker_status)) {
      throw_error(
          "IO_PROJECT_CREATE_MARKER",
          "The project publication marker is invalid.");
    }
    {
      std::ifstream marker(publish_marker, std::ios::binary);
      const std::string marker_text{
          std::istreambuf_iterator<char>(marker),
          std::istreambuf_iterator<char>()};
      if (!marker || marker_text != expected_marker) {
        throw_error(
            "IO_PROJECT_CREATE_MARKER",
            "The project publication marker is invalid.");
      }
    }
    error.clear();
    if (!std::filesystem::remove(publish_marker, error) || error) {
      throw_error(
          "IO_PROJECT_FINALIZE",
          "The recovered project publication marker could not be finalized.");
    }
  } else if (error &&
             error != std::errc::no_such_file_or_directory) {
    throw_error(
        "IO_PROJECT_CREATE_MARKER",
        "The project publication marker could not be inspected.");
  }
  return project;
}

ProjectStore ProjectStore::migrate_v1_to_v2(
    const std::filesystem::path& source_root,
    const std::filesystem::path& target_root,
    FaultPoint fault_point) {
  validate_project_root(source_root);
  validate_project_root(target_root);

  const std::filesystem::path resolved_source =
      resolve_project_root(
          source_root,
          "The migration source path could not be resolved.");
  const std::filesystem::path resolved_target =
      resolve_project_root(
          target_root,
          "The migration target path could not be resolved.");
  std::error_code error;
  require_real_directory_ancestry(resolved_source.parent_path());
  require_real_directory_ancestry(resolved_target.parent_path());
  if (!std::filesystem::equivalent(
          resolved_source.parent_path(),
          resolved_target.parent_path(),
          error) ||
      error) {
    throw_error(
        "IO_MIGRATION_PATH",
        "The migration target must share the source project parent directory.");
  }

  const auto source_status =
      std::filesystem::symlink_status(resolved_source, error);
  if (error || !std::filesystem::is_directory(source_status) ||
      is_link_or_reparse_point(resolved_source, source_status)) {
    throw_error(
        "IO_PROJECT_OPEN",
        "The migration source project is unavailable.");
  }
  const auto source_database_status = std::filesystem::symlink_status(
      resolved_source / "project.db", error);
  if (error || !std::filesystem::is_regular_file(source_database_status) ||
      is_link_or_reparse_point(
          resolved_source / "project.db", source_database_status)) {
    throw_error(
        "IO_DATABASE_OPEN",
        "The migration source database is unavailable.");
  }

  auto source_lock =
      std::make_unique<ProjectLock>(resolved_source / "project.lock");
  auto source_database = std::make_unique<Database>(
      resolved_source / "project.db",
      SQLITE_OPEN_READONLY | SQLITE_OPEN_EXRESCODE |
          SQLITE_OPEN_NOFOLLOW);
  configure_read_only_database(*source_database);
  static_cast<void>(validate_database_format(
      *source_database, kProjectUserVersionV1));
  const std::int64_t source_clean_state =
      get_meta_integer(*source_database, "clean_shutdown");
  if (source_clean_state != 0 && source_clean_state != 1) {
    throw_error(
        "IO_PROJECT_FORMAT",
        "The source clean-shutdown marker is invalid.");
  }
  if (source_clean_state == 0) {
    check_database_integrity(*source_database);
  }

  auto source_implementation = std::make_unique<Impl>(
      resolved_source,
      std::move(source_lock),
      std::move(source_database),
      source_clean_state == 0,
      kProjectUserVersionV1,
      true);
  ProjectStore source(std::move(source_implementation));
  static_cast<void>(source.verify_integrity());
  validate_v1_migration_semantics(*source.impl_->database);
  const std::string source_document_id = source.document_id();
  const std::string source_fingerprint =
      migration_source_fingerprint(*source.impl_->database);

  const bool target_exists = path_entry_exists_no_follow(
      resolved_target,
      "IO_PROJECT_STAT",
      "The migration target could not be inspected.");
  if (target_exists) {
    try {
      ProjectStore existing = ProjectStore::open(resolved_target);
      const bool matches =
          existing.project_format() == kProjectFormatV2 &&
          existing.document_id() == source_document_id &&
          get_meta(
              *existing.impl_->database,
              "migration_source_document_id") == source_document_id &&
          get_meta(
              *existing.impl_->database,
              "migration_source_fingerprint") == source_fingerprint;
      if (!matches) {
        throw_error(
            "IO_MIGRATION_TARGET_CONFLICT",
            "The migration target belongs to a different source state.");
      }
      return existing;
    } catch (const ProjectStoreError& target_error) {
      if (target_error.code() == "IO_MIGRATION_TARGET_CONFLICT" ||
          target_error.code() == "IO_PROJECT_LOCKED") {
        throw;
      }
      throw_error(
          "IO_MIGRATION_TARGET_CONFLICT",
          "The existing migration target is not the expected complete v2 project.");
    }
  }

  const std::filesystem::path staging_root =
      resolved_target.parent_path() /
      (random_identifier(".nps-migrating-") + ".npsproj");
  const bool staging_created =
      std::filesystem::create_directory(staging_root, error);
  if (error || !staging_created) {
    throw_error(
        "IO_MIGRATION_CREATE",
        "The private migration staging directory could not be reserved.");
  }
  OwnedStagingDirectory staging_cleanup(staging_root);
  std::filesystem::create_directories(
      staging_root / "objects" / "sha256", error);
  if (error) {
    throw_error(
        "IO_MIGRATION_CREATE",
        "The migration object directory could not be created.");
  }
  for (const std::string_view child :
       {"previews", "recovery", "manifests"}) {
    std::filesystem::create_directories(staging_root / child, error);
    if (error) {
      throw_error(
          "IO_MIGRATION_CREATE",
          "A migration project directory could not be created.");
    }
  }

  {
    Statement objects(
        source.impl_->database->get(),
        "SELECT hash FROM objects ORDER BY hash;");
    while (objects.row()) {
      const std::string expected_hash = objects.text(0);
      validate_object_ancestry(resolved_source, expected_hash);
      const std::vector<std::uint8_t> bytes =
          read_file(object_path(resolved_source, expected_hash));
      if (sha256(bytes) != expected_hash ||
          persist_object(staging_root, bytes) != expected_hash) {
        throw_error(
            "IO_OBJECT_CORRUPT",
            "A source object changed during migration.");
      }
    }
  }

  {
    auto migrated_database = std::make_unique<Database>(
        staging_root / "project.db",
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_EXRESCODE |
            SQLITE_OPEN_NOFOLLOW);
    backup_database(
        *source.impl_->database, *migrated_database);
    configure_database(*migrated_database);
    static_cast<void>(validate_database_format(
        *migrated_database, kProjectUserVersionV1));
    if (migration_source_fingerprint(*migrated_database) !=
        source_fingerprint) {
      throw_error(
          "IO_MIGRATION_SOURCE_CHANGED",
          "The migration source changed after validation.");
    }
    transform_database_v1_to_v2(
        *migrated_database, source_fingerprint);
    try {
      migrated_database->exec("PRAGMA wal_checkpoint(TRUNCATE);");
    } catch (...) {
    }
  }

  {
    ProjectStore staged = ProjectStore::open(staging_root);
    if (staged.project_format() != kProjectFormatV2 ||
        staged.document_id() != source_document_id ||
        get_meta(
            *staged.impl_->database,
            "migration_source_fingerprint") != source_fingerprint ||
        migration_source_fingerprint(
            *staged.impl_->database, true) != source_fingerprint) {
      throw_error(
          "IO_MIGRATION_VERIFY",
          "The staged v2 project does not match its source.");
    }
    static_cast<void>(staged.verify_integrity());
    staged.close();
  }
  sync_file(staging_root / "project.db");
  {
    std::ofstream marker(
        staging_root / ".nps-migrating",
        std::ios::binary | std::ios::trunc);
    marker << "nps.migrate/v1-to-v2\n";
    marker.flush();
    if (!marker) {
      throw_error(
          "IO_MIGRATION_CREATE",
          "The migration publication marker could not be written.");
    }
  }
  sync_file(staging_root / ".nps-migrating");
  inject_crash_if_requested(
      fault_point, FaultPoint::after_migration_staged);

  if (path_entry_exists_no_follow(
          resolved_target,
          "IO_PROJECT_STAT",
          "The migration target could not be inspected before publication.")) {
    throw_error(
        "IO_MIGRATION_TARGET_CONFLICT",
        "Another process published the migration target first.");
  }
  std::filesystem::rename(staging_root, resolved_target, error);
  if (error) {
    if (path_entry_exists_no_follow(
            resolved_target,
            "IO_PROJECT_STAT",
            "The migration target could not be inspected after publication.")) {
      throw_error(
          "IO_MIGRATION_TARGET_CONFLICT",
          "Another process published the migration target first.");
    }
    throw_error(
        "IO_MIGRATION_PUBLISH",
        "The completed v2 project could not be published atomically.");
  }
  staging_cleanup.release_after_publish();
  inject_crash_if_requested(
      fault_point, FaultPoint::after_migration_published);
  return ProjectStore::open(resolved_target);
}

ProjectStore::ProjectStore(ProjectStore&&) noexcept = default;

ProjectStore& ProjectStore::operator=(ProjectStore&& other) noexcept {
  if (this != &other) {
    try {
      close();
    } catch (...) {
    }
    impl_ = std::move(other.impl_);
  }
  return *this;
}

ProjectStore::~ProjectStore() {
  try {
    close();
  } catch (...) {
  }
}

std::string ProjectStore::document_id() const {
  if (!impl_) {
    throw_error("IO_PROJECT_CLOSED", "The project is closed.");
  }
  return get_meta(*impl_->database, "document_id");
}

std::string ProjectStore::project_format() const {
  if (!impl_) {
    throw_error("IO_PROJECT_CLOSED", "The project is closed.");
  }
  return impl_->format_version == kProjectUserVersionV2
             ? std::string(kProjectFormatV2)
             : std::string(kProjectFormatV1);
}

std::int64_t ProjectStore::current_revision() const {
  if (!impl_) {
    throw_error("IO_PROJECT_CLOSED", "The project is closed.");
  }
  return get_meta_integer(*impl_->database, "current_revision");
}

Snapshot ProjectStore::current_snapshot() const {
  if (!impl_) {
    throw_error("IO_PROJECT_CLOSED", "The project is closed.");
  }
  return read_snapshot(
      *impl_->database,
      get_meta_integer(*impl_->database, "current_snapshot_id"),
      impl_->format_version);
}

std::vector<std::uint8_t> ProjectStore::read_source_bytes() const {
  if (!impl_) {
    throw_error("IO_PROJECT_CLOSED", "The project is closed.");
  }
  const Snapshot snapshot = current_snapshot();
  validate_object_ancestry(impl_->root, snapshot.source_hash);
  const auto bytes = read_file(object_path(impl_->root, snapshot.source_hash));
  if (sha256(bytes) != snapshot.source_hash) {
    throw_error("IO_OBJECT_CORRUPT", "The source object failed verification.");
  }
  return bytes;
}

IntegrityReport ProjectStore::verify_integrity() const {
  if (!impl_) {
    throw_error("IO_PROJECT_CLOSED", "The project is closed.");
  }
  static_cast<void>(validate_database_format(
      *impl_->database, impl_->format_version));
  check_database_integrity(*impl_->database);

  const std::int64_t revision =
      get_meta_integer(*impl_->database, "current_revision");
  const std::int64_t current_snapshot_id =
      get_meta_integer(*impl_->database, "current_snapshot_id");
  const std::int64_t history_position =
      get_meta_integer(*impl_->database, "history_position");
  if (revision < 0 || history_position < 0) {
    throw_error("IO_PROJECT_FORMAT", "Project revision metadata is invalid.");
  }
  static_cast<void>(read_snapshot(
      *impl_->database, current_snapshot_id, impl_->format_version));
  if (impl_->format_version == kProjectUserVersionV2) {
    Statement snapshots(
        impl_->database->get(), "SELECT id FROM snapshots ORDER BY id;");
    std::size_t snapshot_count = 0U;
    while (snapshots.row()) {
      static_cast<void>(read_snapshot(
          *impl_->database,
          snapshots.integer(0),
          impl_->format_version));
      ++snapshot_count;
    }
    if (snapshot_count == 0U ||
        get_meta(*impl_->database, "migration_source_format") !=
            kProjectFormatV1 ||
        get_meta(*impl_->database, "migration_source_document_id").empty() ||
        !is_lower_hex(
            get_meta(*impl_->database, "migration_source_fingerprint"))) {
      throw_error(
          "IO_PROJECT_FORMAT",
          "The v2 migration provenance is invalid.");
    }
  }
  if (history_snapshot(*impl_->database, history_position) !=
      current_snapshot_id) {
    throw_error(
        "IO_PROJECT_FORMAT",
        "The current snapshot and history cursor disagree.");
  }
  {
    Statement history_shape(
        impl_->database->get(),
        "SELECT COUNT(*), MIN(position), MAX(position) FROM history;");
    if (!history_shape.row()) {
      throw_error("IO_PROJECT_FORMAT", "The project history is missing.");
    }
    const std::int64_t count = history_shape.integer(0);
    const std::int64_t minimum = history_shape.integer(1);
    const std::int64_t maximum = history_shape.integer(2);
    if (count <= 0 || minimum != 0 || maximum != count - 1 ||
        history_position > maximum) {
      throw_error("IO_PROJECT_FORMAT", "The project history is not contiguous.");
    }
  }
  {
    Statement transaction_shape(
        impl_->database->get(),
        "SELECT COUNT(*), COALESCE(MAX(revision), 0) FROM transactions;");
    if (!transaction_shape.row() ||
        transaction_shape.integer(0) != revision ||
        transaction_shape.integer(1) != revision) {
      throw_error(
          "IO_PROJECT_FORMAT",
          "The transaction log and current revision disagree.");
    }
  }

  {
    Statement objects(
        impl_->database->get(),
        "SELECT hash, byte_size FROM objects ORDER BY hash;");
    bool found_object = false;
    while (objects.row()) {
      found_object = true;
      const std::string hash = objects.text(0);
      const std::int64_t recorded_size = objects.integer(1);
      validate_object_ancestry(impl_->root, hash);
      const auto bytes = read_file(object_path(impl_->root, hash));
      if (recorded_size <= 0 ||
          static_cast<std::uintmax_t>(recorded_size) != bytes.size() ||
          sha256(bytes) != hash) {
        throw_error(
            "IO_OBJECT_CORRUPT",
            "An object does not match its database record.");
      }
    }
    if (!found_object) {
      throw_error("IO_PROJECT_FORMAT", "The project object table is empty.");
    }
  }

  std::set<std::string> referenced;
  Statement references(
      impl_->database->get(),
      "SELECT DISTINCT source_hash FROM snapshots ORDER BY source_hash;");
  while (references.row()) {
    const std::string hash = references.text(0);
    if (!is_lower_hex(hash)) {
      throw_error("IO_OBJECT_HASH", "A referenced object hash is invalid.");
    }
    validate_object_ancestry(impl_->root, hash);
    const auto bytes = read_file(object_path(impl_->root, hash));
    if (sha256(bytes) != hash) {
      throw_error("IO_OBJECT_CORRUPT", "A referenced object failed verification.");
    }
    referenced.insert(hash);
  }
  if (referenced.empty()) {
    throw_error("IO_PROJECT_FORMAT", "The project contains no source object.");
  }

  std::size_t orphan_count = 0;
  const std::filesystem::path object_root =
      impl_->root / "objects" / "sha256";
  require_real_directory(impl_->root);
  require_real_directory(impl_->root / "objects");
  require_real_directory(object_root);
  std::error_code error;
  for (std::filesystem::recursive_directory_iterator iterator(
           object_root, std::filesystem::directory_options::none, error),
       end;
       iterator != end;
       iterator.increment(error)) {
    if (error) {
      throw_error("IO_OBJECT_STAT", "The object store could not be inspected.");
    }
    const auto status = iterator->symlink_status(error);
    if (error || is_link_or_reparse_point(iterator->path(), status)) {
      throw_error("IO_OBJECT_LINK", "Symbolic links are not allowed in projects.");
    }
    if (!std::filesystem::is_regular_file(status)) {
      continue;
    }
    const auto relative = iterator->path().lexically_relative(object_root);
    auto component = relative.begin();
    if (component == relative.end()) {
      throw_error("IO_OBJECT_HASH", "An object-store path is invalid.");
    }
    const std::string parent = path_to_utf8(*component);
    ++component;
    if (component == relative.end()) {
      throw_error("IO_OBJECT_HASH", "An object-store path is invalid.");
    }
    const std::string filename = path_to_utf8(*component);
    ++component;
    if (component != relative.end() || parent.size() != 2U ||
        filename.size() != 62U) {
      throw_error("IO_OBJECT_HASH", "An object-store path is invalid.");
    }
    const std::string hash = parent + filename;
    if (!is_lower_hex(hash)) {
      throw_error("IO_OBJECT_HASH", "An object-store filename is invalid.");
    }
    if (!referenced.contains(hash)) {
      ++orphan_count;
    }
  }
  if (error) {
    throw_error("IO_OBJECT_STAT", "The object store could not be inspected.");
  }

  return IntegrityReport{
      .revision = revision,
      .referenced_objects = referenced.size(),
      .orphan_objects = orphan_count,
      .recovered_unclean_shutdown = impl_->recovered_unclean_shutdown};
}

CommitResult ProjectStore::execute(
    const StoreCommand& command,
    FaultPoint fault_point) {
  if (!impl_) {
    throw_error("IO_PROJECT_CLOSED", "The project is closed.");
  }
  if (command.command_id.empty() || command.idempotency_key.empty() ||
      command.request_fingerprint.empty()) {
    throw_error("CMD_SCHEMA_INVALID", "Required command identifiers are empty.");
  }
  if (command.expected_revision < 0) {
    throw_error("CMD_SCHEMA_INVALID", "The expected revision cannot be negative.");
  }
  if (command.mutation == StoreMutation::adjust_exposure &&
      (!std::isfinite(command.exposure_delta_ev) ||
       command.exposure_delta_ev < -10.0 ||
       command.exposure_delta_ev > 10.0)) {
    throw_error("CMD_SCHEMA_INVALID", "The exposure value is outside the M0 range.");
  }
  if (command.mutation == StoreMutation::replace_graph) {
    if (impl_->format_version != kProjectUserVersionV2) {
      throw_error(
          "IO_PROJECT_MIGRATION_REQUIRED",
          "graph.replace requires an explicit migration to nps.project/v2.");
    }
    if (!is_lower_hex(command.edit_graph_sha256) ||
        sha256(command.edit_graph_json) != command.edit_graph_sha256 ||
        !validate_canonical_edit_graph(
            command.edit_graph_json, command.working_color_id)) {
      throw_error(
          "CMD_SCHEMA_INVALID",
          "The replacement edit graph is not canonical or valid.");
    }
  }

  Database& database = *impl_->database;
  bool commit_requested = false;
  database.exec("BEGIN IMMEDIATE;");
  try {
    struct ReplayRecord {
      std::string fingerprint;
      std::string command_id;
      std::int64_t base_revision{};
      std::int64_t new_revision{};
      std::int64_t snapshot_id{};
    };
    std::optional<ReplayRecord> replay_record;
    {
      Statement replay(
          database.get(),
          "SELECT request_fingerprint, command_id, base_revision, "
          "new_revision, snapshot_id "
          "FROM idempotency WHERE idempotency_key = ?1;");
      replay.bind(1, command.idempotency_key);
      if (replay.row()) {
        replay_record = ReplayRecord{
            .fingerprint = replay.text(0),
            .command_id = replay.text(1),
            .base_revision = replay.integer(2),
            .new_revision = replay.integer(3),
            .snapshot_id = replay.integer(4)};
        if (replay.row()) {
          throw_error(
              "IO_PROJECT_FORMAT",
              "The idempotency record is ambiguous.");
        }
      }
    }
    if (replay_record) {
      if (replay_record->fingerprint != command.request_fingerprint) {
        throw_error(
            "CMD_IDEMPOTENCY_REUSE",
            "The idempotency key was reused for a different command.");
      }
      CommitResult result{
          .command_id = replay_record->command_id,
          .base_revision = replay_record->base_revision,
          .new_revision = replay_record->new_revision,
          .snapshot = read_snapshot(
              database, replay_record->snapshot_id, impl_->format_version),
          .idempotent_replay = true};
      database.exec("ROLLBACK;");
      return result;
    }

    const std::int64_t base_revision =
        get_meta_integer(database, "current_revision");
    if (base_revision < 0) {
      throw_error("IO_PROJECT_FORMAT", "The current revision is invalid.");
    }
    if (command.expected_revision != base_revision) {
      throw_error(
          "REV_CONFLICT",
          "The command expected a different project revision.");
    }
    if (base_revision == std::numeric_limits<std::int64_t>::max()) {
      throw_error(
          "REV_LIMIT",
          "The project reached the maximum supported revision.");
    }
    const std::int64_t new_revision = base_revision + 1;
    std::int64_t history_position =
        get_meta_integer(database, "history_position");
    std::int64_t snapshot_id =
        get_meta_integer(database, "current_snapshot_id");

    if (command.mutation == StoreMutation::adjust_exposure ||
        command.mutation == StoreMutation::replace_graph) {
      const Snapshot current =
          read_snapshot(database, snapshot_id, impl_->format_version);
      const double exposure_ev =
          command.mutation == StoreMutation::adjust_exposure
              ? current.exposure_ev + command.exposure_delta_ev
              : current.exposure_ev;
      if (!std::isfinite(exposure_ev)) {
        throw_error("CMD_SCHEMA_INVALID", "The resulting exposure is invalid.");
      }

      if (impl_->format_version == kProjectUserVersionV2) {
        std::string edit_graph_json = command.edit_graph_json;
        std::string edit_graph_hash = command.edit_graph_sha256;
        std::string working_color_id = command.working_color_id;
        if (command.mutation == StoreMutation::adjust_exposure) {
          const std::string revision_text = std::to_string(new_revision);
          edit_graph_json = append_exposure_to_graph(
              current.edit_graph_json,
              "graph-revision-" + revision_text,
              "adjust-exposure-revision-" + revision_text,
              command.exposure_delta_ev);
          edit_graph_hash = sha256(edit_graph_json);
          working_color_id = current.working_color_id;
        }
        if (!validate_canonical_edit_graph(
                edit_graph_json, working_color_id) ||
            sha256(edit_graph_json) != edit_graph_hash) {
          throw_error(
              "IO_PROJECT_FORMAT",
              "The new snapshot edit graph is invalid.");
        }
        Statement snapshot_insert(
            database.get(),
            "INSERT INTO snapshots("
            "parent_snapshot_id, created_revision, source_hash, exposure_ev, "
            "edit_graph_json, edit_graph_sha256, working_color_id"
            ") VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7);");
        snapshot_insert.bind(1, current.id);
        snapshot_insert.bind(2, new_revision);
        snapshot_insert.bind(3, current.source_hash);
        snapshot_insert.bind(4, exposure_ev);
        snapshot_insert.bind(5, edit_graph_json);
        snapshot_insert.bind(6, edit_graph_hash);
        snapshot_insert.bind(7, working_color_id);
        snapshot_insert.done();
      } else {
        Statement snapshot_insert(
            database.get(),
            "INSERT INTO snapshots("
            "parent_snapshot_id, created_revision, source_hash, exposure_ev"
            ") VALUES(?1, ?2, ?3, ?4);");
        snapshot_insert.bind(1, current.id);
        snapshot_insert.bind(2, new_revision);
        snapshot_insert.bind(3, current.source_hash);
        snapshot_insert.bind(4, exposure_ev);
        snapshot_insert.done();
      }
      snapshot_id = sqlite3_last_insert_rowid(database.get());

      Statement truncate_history(
          database.get(), "DELETE FROM history WHERE position > ?1;");
      truncate_history.bind(1, history_position);
      truncate_history.done();
      if (history_position == std::numeric_limits<std::int64_t>::max()) {
        throw_error(
            "REV_LIMIT",
            "The project reached the maximum supported history position.");
      }
      ++history_position;
      Statement history_insert(
          database.get(),
          "INSERT INTO history(position, snapshot_id) VALUES(?1, ?2);");
      history_insert.bind(1, history_position);
      history_insert.bind(2, snapshot_id);
      history_insert.done();
    } else if (command.mutation == StoreMutation::undo) {
      if (history_position <= 0) {
        throw_error("CMD_HISTORY_BOUNDARY", "There is no edit to undo.");
      }
      --history_position;
      snapshot_id = history_snapshot(database, history_position);
    } else if (command.mutation == StoreMutation::redo) {
      if (history_position >= maximum_history_position(database)) {
        throw_error("CMD_HISTORY_BOUNDARY", "There is no edit to redo.");
      }
      ++history_position;
      snapshot_id = history_snapshot(database, history_position);
    } else {
      throw_error("CMD_UNSUPPORTED_TYPE", "The command kind is not supported.");
    }

    const std::string kind = mutation_name(command.mutation);
    Statement transaction_insert(
        database.get(),
        "INSERT INTO transactions("
        "revision, base_revision, command_id, idempotency_key, "
        "request_fingerprint, kind, snapshot_id"
        ") VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7);");
    transaction_insert.bind(1, new_revision);
    transaction_insert.bind(2, base_revision);
    transaction_insert.bind(3, command.command_id);
    transaction_insert.bind(4, command.idempotency_key);
    transaction_insert.bind(5, command.request_fingerprint);
    transaction_insert.bind(6, kind);
    transaction_insert.bind(7, snapshot_id);
    transaction_insert.done();

    Statement idempotency_insert(
        database.get(),
        "INSERT INTO idempotency("
        "idempotency_key, request_fingerprint, command_id, "
        "base_revision, new_revision, snapshot_id"
        ") VALUES(?1, ?2, ?3, ?4, ?5, ?6);");
    idempotency_insert.bind(1, command.idempotency_key);
    idempotency_insert.bind(2, command.request_fingerprint);
    idempotency_insert.bind(3, command.command_id);
    idempotency_insert.bind(4, base_revision);
    idempotency_insert.bind(5, new_revision);
    idempotency_insert.bind(6, snapshot_id);
    idempotency_insert.done();

    set_meta_integer(database, "current_revision", new_revision);
    set_meta_integer(database, "current_snapshot_id", snapshot_id);
    set_meta_integer(database, "history_position", history_position);
    CommitResult result{
        .command_id = command.command_id,
        .base_revision = base_revision,
        .new_revision = new_revision,
        .snapshot =
            read_snapshot(database, snapshot_id, impl_->format_version),
        .idempotent_replay = false};
    commit_requested = true;
    database.exec("COMMIT;");

    inject_crash_if_requested(
        fault_point, FaultPoint::after_database_committed);
    return result;
  } catch (...) {
    const bool transaction_is_active =
        sqlite3_get_autocommit(database.get()) == 0;
    if (transaction_is_active) {
      rollback_noexcept(database);
    } else if (commit_requested) {
      throw ProjectStoreError(
          "IO_COMMIT_RESULT_UNKNOWN",
          "The commit outcome is uncertain; retry with the same idempotency key.");
    }
    throw;
  }
}

void ProjectStore::close() {
  if (!impl_) {
    return;
  }
  auto closing = std::move(impl_);
  if (closing->read_only) {
    return;
  }
  Database& database = *closing->database;
  database.exec("BEGIN IMMEDIATE;");
  try {
    set_meta_integer(database, "clean_shutdown", 1);
    database.exec("COMMIT;");
  } catch (...) {
    rollback_noexcept(database);
    throw;
  }
  try {
    database.exec("PRAGMA wal_checkpoint(TRUNCATE);");
  } catch (...) {
    // A checkpoint is an optimization, not project truth. The committed WAL
    // remains recoverable, and the connection is closed before returning.
  }
}

}  // namespace nps::core
