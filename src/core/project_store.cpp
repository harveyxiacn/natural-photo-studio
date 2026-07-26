#include "nps/core/project_store.hpp"

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
#include <utility>

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
constexpr int kProjectUserVersion = 1;
constexpr int kInjectedCrashExitCode = 86;
constexpr std::size_t kSha256Bytes = 32;
constexpr std::size_t kSha256HexCharacters = kSha256Bytes * 2;
constexpr std::uintmax_t kMaximumObjectBytes =
    static_cast<std::uintmax_t>(8) * 1024U * 1024U * 1024U;

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
  Statement(sqlite3* database, std::string_view sql) : database_(database) {
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

 private:
  sqlite3* database_{};
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

[[nodiscard]] Snapshot read_snapshot(
    const Database& database,
    std::int64_t snapshot_id) {
  Statement statement(
      database.get(),
      "SELECT id, created_revision, source_hash, exposure_ev "
      "FROM snapshots WHERE id = ?1;");
  statement.bind(1, snapshot_id);
  if (!statement.row()) {
    throw_error("IO_PROJECT_FORMAT", "The current snapshot is missing.");
  }
  Snapshot snapshot{
      .id = statement.integer(0),
      .created_revision = statement.integer(1),
      .source_hash = statement.text(2),
      .exposure_ev = statement.real(3)};
  if (statement.row()) {
    throw_error("IO_PROJECT_FORMAT", "The current snapshot is ambiguous.");
  }
  if (!is_lower_hex(snapshot.source_hash) ||
      !std::isfinite(snapshot.exposure_ev)) {
    throw_error("IO_PROJECT_FORMAT", "The current snapshot is invalid.");
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

}  // namespace

struct ProjectStore::Impl {
  Impl(
      std::filesystem::path root_value,
      std::unique_ptr<ProjectLock> lock_value,
      std::unique_ptr<Database> database_value,
      bool recovered_value)
      : root(std::move(root_value)),
        lock(std::move(lock_value)),
        database(std::move(database_value)),
        recovered_unclean_shutdown(recovered_value) {}

  std::filesystem::path root;
  std::unique_ptr<ProjectLock> lock;
  std::unique_ptr<Database> database;
  bool recovered_unclean_shutdown{};
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

  std::error_code error;
  const std::filesystem::path published_root =
      std::filesystem::absolute(project_root, error).lexically_normal();
  if (error) {
    throw_error("IO_PROJECT_PATH", "The destination path could not be resolved.");
  }
  if (std::filesystem::exists(published_root, error)) {
    throw_error("IO_PROJECT_EXISTS", "The destination project already exists.");
  }
  if (error) {
    throw_error("IO_PROJECT_STAT", "The destination project could not be inspected.");
  }

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

    set_meta(project_database, "format", "nps.project/v1");
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
        std::to_string(kProjectUserVersion) + ";");
    project_database.exec("COMMIT;");
  } catch (...) {
    rollback_noexcept(project_database);
    throw;
  }

  try {
    project_database.exec("PRAGMA wal_checkpoint(TRUNCATE);");
  } catch (...) {
  }
  database.reset();
  std::filesystem::rename(staging_root, published_root, error);
  if (error) {
    if (std::filesystem::exists(published_root)) {
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
  std::error_code error;
  const auto root_status = std::filesystem::symlink_status(project_root, error);
  if (error || !std::filesystem::is_directory(root_status) ||
      is_link_or_reparse_point(project_root, root_status)) {
    throw_error("IO_PROJECT_OPEN", "The project directory is unavailable.");
  }
  const auto database_status =
      std::filesystem::symlink_status(project_root / "project.db", error);
  if (error || !std::filesystem::is_regular_file(database_status) ||
      is_link_or_reparse_point(project_root / "project.db", database_status)) {
    throw_error("IO_DATABASE_OPEN", "The project database is unavailable.");
  }

  auto project_lock =
      std::make_unique<ProjectLock>(project_root / "project.lock");
  auto database = std::make_unique<Database>(
      project_root / "project.db",
      SQLITE_OPEN_READWRITE | SQLITE_OPEN_EXRESCODE |
          SQLITE_OPEN_NOFOLLOW);
  configure_database(*database);

  {
    Statement application_id(database->get(), "PRAGMA application_id;");
    if (!application_id.row() ||
        application_id.integer(0) != kProjectApplicationId) {
      throw_error(
          "IO_PROJECT_FORMAT",
          "The project application identifier is invalid.");
    }
  }
  {
    Statement user_version(database->get(), "PRAGMA user_version;");
    if (!user_version.row() ||
        user_version.integer(0) != kProjectUserVersion) {
      throw_error(
          "IO_PROJECT_VERSION",
          "The project format version is unsupported.");
    }
  }
  if (get_meta(*database, "format") != "nps.project/v1") {
    throw_error("IO_PROJECT_VERSION", "The project format version is unsupported.");
  }

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
      project_root,
      std::move(project_lock),
      std::move(database),
      recovered);
  Database& project_database = *implementation->database;
  project_database.exec("BEGIN IMMEDIATE;");
  try {
    set_meta_integer(project_database, "clean_shutdown", 0);
    project_database.exec("COMMIT;");
  } catch (...) {
    rollback_noexcept(project_database);
    throw;
  }

  ProjectStore project(std::move(implementation));
  static_cast<void>(project.verify_integrity());
  const std::filesystem::path create_marker =
      project_root / ".nps-creating";
  const auto marker_status =
      std::filesystem::symlink_status(create_marker, error);
  if (!error && std::filesystem::exists(marker_status)) {
    if (!std::filesystem::is_regular_file(marker_status) ||
        is_link_or_reparse_point(create_marker, marker_status)) {
      throw_error(
          "IO_PROJECT_CREATE_MARKER",
          "The project creation marker is invalid.");
    }
    {
      std::ifstream marker(create_marker, std::ios::binary);
      const std::string marker_text{
          std::istreambuf_iterator<char>(marker),
          std::istreambuf_iterator<char>()};
      if (!marker || marker_text != "nps.create/v1\n") {
        throw_error(
            "IO_PROJECT_CREATE_MARKER",
            "The project creation marker is invalid.");
      }
    }
    error.clear();
    if (!std::filesystem::remove(create_marker, error) || error) {
      throw_error(
          "IO_PROJECT_FINALIZE",
          "The recovered project marker could not be finalized.");
    }
  } else if (error &&
             error != std::errc::no_such_file_or_directory) {
    throw_error(
        "IO_PROJECT_CREATE_MARKER",
        "The project creation marker could not be inspected.");
  }
  return project;
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
      get_meta_integer(*impl_->database, "current_snapshot_id"));
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
  static_cast<void>(read_snapshot(*impl_->database, current_snapshot_id));
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
          .snapshot = read_snapshot(database, replay_record->snapshot_id),
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

    if (command.mutation == StoreMutation::adjust_exposure) {
      const Snapshot current = read_snapshot(database, snapshot_id);
      const double exposure_ev =
          current.exposure_ev + command.exposure_delta_ev;
      if (!std::isfinite(exposure_ev)) {
        throw_error("CMD_SCHEMA_INVALID", "The resulting exposure is invalid.");
      }

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
        .snapshot = read_snapshot(database, snapshot_id),
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
