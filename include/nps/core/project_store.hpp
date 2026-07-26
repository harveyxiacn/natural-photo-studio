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
  undo,
  redo,
};

enum class FaultPoint {
  none,
  after_object_persisted,
  after_database_committed,
};

struct StoreCommand {
  std::string command_id;
  std::string idempotency_key;
  std::string request_fingerprint;
  std::int64_t expected_revision{};
  StoreMutation mutation{StoreMutation::adjust_exposure};
  double exposure_delta_ev{};
};

struct Snapshot {
  std::int64_t id{};
  std::int64_t created_revision{};
  std::string source_hash;
  double exposure_ev{};
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
  static ProjectStore create(
      const std::filesystem::path& project_root,
      std::span<const std::uint8_t> source_bytes,
      FaultPoint fault_point = FaultPoint::none);

  static ProjectStore open(const std::filesystem::path& project_root);

  ProjectStore(ProjectStore&&) noexcept;
  ProjectStore& operator=(ProjectStore&&) noexcept;
  ProjectStore(const ProjectStore&) = delete;
  ProjectStore& operator=(const ProjectStore&) = delete;
  ~ProjectStore();

  [[nodiscard]] std::string document_id() const;
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
