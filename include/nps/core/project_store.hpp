#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace nps::core {

enum class StoreMutation {
  adjust_exposure,
  replace_graph,
  undo,
  redo,
};

enum class FaultPoint {
  none,
  after_object_persisted,
  after_database_committed,
  after_migration_staged,
  after_migration_published,
};

struct StoreCommand {
  std::string command_id;
  std::string idempotency_key;
  std::string request_fingerprint;
  std::int64_t expected_revision{};
  StoreMutation mutation{StoreMutation::adjust_exposure};
  double exposure_delta_ev{};
  std::string edit_graph_json{};
  std::string edit_graph_sha256{};
  std::string working_color_id{};
};

struct Snapshot {
  std::int64_t id{};
  std::int64_t created_revision{};
  std::string source_hash;
  double exposure_ev{};
  // Empty for nps.project/v1. nps.project/v2 stores the complete canonical
  // immutable graph and its explicit color contract on every snapshot.
  std::string edit_graph_json{};
  std::string edit_graph_sha256{};
  std::string working_color_id{};
};

struct CommitResult {
  std::string command_id;
  std::int64_t base_revision{};
  std::int64_t new_revision{};
  Snapshot snapshot;
  bool idempotent_replay{};
};

struct IntegrityReport {
  std::int64_t revision{};
  std::size_t referenced_objects{};
  std::size_t orphan_objects{};
  bool recovered_unclean_shutdown{};
};

class ProjectStoreError final : public std::runtime_error {
 public:
  ProjectStoreError(std::string code, std::string message);

  [[nodiscard]] const std::string& code() const noexcept;

 private:
  std::string code_;
};

class ProjectStore final {
 public:
  // Project paths are captured as absolute identities before the store is
  // returned. On POSIX, only the existing parent is canonicalized so benign
  // system aliases are accepted without following the final .npsproj
  // component. The final project directory and project.db must remain real
  // filesystem objects; symbolic links and Windows reparse points are
  // rejected, and SQLite database opens retain SQLITE_OPEN_NOFOLLOW.
  //
  // These checks reduce accidental link traversal but do not form a sandbox
  // against an equal-privilege process replacing path entries concurrently.
  static ProjectStore create(
      const std::filesystem::path& project_root,
      std::span<const std::uint8_t> source_bytes,
      FaultPoint fault_point = FaultPoint::none);

  static ProjectStore open(const std::filesystem::path& project_root);

  // Explicitly creates a separate v2 project. The v1 source is opened
  // read-only under its project lease and is never replaced. source_root and
  // target_root must be distinct siblings so the completed staging directory
  // can be published with one rename.
  static ProjectStore migrate_v1_to_v2(
      const std::filesystem::path& source_root,
      const std::filesystem::path& target_root,
      FaultPoint fault_point = FaultPoint::none);

  ProjectStore(ProjectStore&&) noexcept;
  ProjectStore& operator=(ProjectStore&&) noexcept;
  ProjectStore(const ProjectStore&) = delete;
  ProjectStore& operator=(const ProjectStore&) = delete;
  ~ProjectStore();

  [[nodiscard]] std::string document_id() const;
  [[nodiscard]] std::string project_format() const;
  [[nodiscard]] std::int64_t current_revision() const;
  [[nodiscard]] Snapshot current_snapshot() const;
  [[nodiscard]] std::vector<std::uint8_t> read_source_bytes() const;
  [[nodiscard]] IntegrityReport verify_integrity() const;

  CommitResult execute(
      const StoreCommand& command,
      FaultPoint fault_point = FaultPoint::none);

  void close();

 private:
  struct Impl;
  explicit ProjectStore(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> impl_;
};

}  // namespace nps::core
