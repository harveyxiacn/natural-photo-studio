#include "nps/core/command_bus.hpp"
#include "nps/imaging/ppm16.hpp"
#include "nps/imaging/synthetic.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>
#include <variant>
#include <vector>

namespace {

using nps::core::AdjustExposureParameters;
using nps::core::Command;
using nps::core::CommandBus;
using nps::core::CommandError;
using nps::core::CommandErrorCode;
using nps::core::CommandExecutionResult;
using nps::core::CommandKind;
using nps::core::CommitResult;
using nps::core::HistoryParameters;
using nps::core::ProjectStore;

class TemporaryCommandBusRoot final {
 public:
  TemporaryCommandBusRoot() {
    static std::atomic_uint64_t sequence{};
    const auto timestamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("nps-command-bus-" + std::to_string(timestamp) + "-" +
             std::to_string(sequence.fetch_add(std::uint64_t{1})));
    if (!std::filesystem::create_directory(path_)) {
      throw std::runtime_error{"unable to create command-bus test directory"};
    }
  }

  ~TemporaryCommandBusRoot() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  TemporaryCommandBusRoot(const TemporaryCommandBusRoot&) = delete;
  TemporaryCommandBusRoot& operator=(const TemporaryCommandBusRoot&) = delete;

  [[nodiscard]] std::filesystem::path project() const {
    return path_ / "test.npsproj";
  }

 private:
  std::filesystem::path path_;
};

[[nodiscard]] std::vector<std::uint8_t> synthetic_ppm_source() {
  const auto encoded = nps::imaging::encode_ppm16(
      nps::imaging::make_deterministic_gradient(13, 7));
  std::vector<std::uint8_t> source;
  source.reserve(encoded.size());
  for (const std::byte value : encoded) {
    source.push_back(std::to_integer<std::uint8_t>(value));
  }
  return source;
}

[[nodiscard]] Command exposure_command(
    const ProjectStore& project,
    std::uint64_t expected_revision,
    std::string token,
    double ev) {
  return Command{
      .command_id = "cmd-" + token,
      .idempotency_key = "idempotency-" + token,
      .document_id = project.document_id(),
      .expected_revision = expected_revision,
      .kind = CommandKind::AdjustExposure,
      .parameters = AdjustExposureParameters{.ev = ev},
      .privacy = {}};
}

[[nodiscard]] Command history_command(
    const ProjectStore& project,
    CommandKind kind,
    std::uint64_t expected_revision,
    std::string token) {
  return Command{
      .command_id = "cmd-" + token,
      .idempotency_key = "idempotency-" + token,
      .document_id = project.document_id(),
      .expected_revision = expected_revision,
      .kind = kind,
      .parameters = HistoryParameters{},
      .privacy = {}};
}

void check_project_unchanged(
    const ProjectStore& project,
    std::int64_t expected_revision,
    const nps::core::Snapshot& expected_snapshot,
    const std::vector<std::uint8_t>& expected_source) {
  CHECK(project.current_revision() == expected_revision);
  const auto current = project.current_snapshot();
  CHECK(current.id == expected_snapshot.id);
  CHECK(current.created_revision == expected_snapshot.created_revision);
  CHECK(current.source_hash == expected_snapshot.source_hash);
  CHECK(current.exposure_ev == expected_snapshot.exposure_ev);
  CHECK(project.read_source_bytes() == expected_source);
}

}  // namespace

TEST_CASE("CommandBus maps a valid exposure command to an atomic commit") {
  TemporaryCommandBusRoot temporary;
  const auto source = synthetic_ppm_source();
  auto project = ProjectStore::create(temporary.project(), source);
  const auto initial = project.current_snapshot();
  CommandBus bus{project};

  const Command command = exposure_command(project, 0, "exposure", 1.0);
  const CommandExecutionResult result = bus.execute(command);

  REQUIRE(std::holds_alternative<CommitResult>(result));
  const CommitResult& committed = std::get<CommitResult>(result);
  CHECK(committed.command_id == command.command_id);
  CHECK(committed.base_revision == 0);
  CHECK(committed.new_revision == 1);
  CHECK_FALSE(committed.idempotent_replay);
  CHECK(committed.snapshot.created_revision == 1);
  CHECK(committed.snapshot.source_hash == initial.source_hash);
  CHECK(committed.snapshot.exposure_ev == 1.0);
  CHECK(project.current_revision() == 1);
  CHECK(project.current_snapshot().id == committed.snapshot.id);
  CHECK(project.read_source_bytes() == source);
}

TEST_CASE("CommandBus rejects a different document without modifying project") {
  TemporaryCommandBusRoot temporary;
  const auto source = synthetic_ppm_source();
  auto project = ProjectStore::create(temporary.project(), source);
  const auto initial = project.current_snapshot();
  CommandBus bus{project};

  Command command = exposure_command(project, 0, "wrong-document", 1.0);
  command.document_id = "doc-not-this-project";
  const CommandExecutionResult result = bus.execute(command);

  REQUIRE(std::holds_alternative<CommandError>(result));
  const CommandError& error = std::get<CommandError>(result);
  CHECK(error.code == CommandErrorCode::CmdSchemaInvalid);
  CHECK(nps::core::to_string(error.code) == "CMD_SCHEMA_INVALID");
  CHECK_FALSE(error.project_modified);
  CHECK_FALSE(error.current_revision.has_value());
  check_project_unchanged(project, 0, initial, source);
}

TEST_CASE("CommandBus maps stale revision to REV_CONFLICT with current revision") {
  TemporaryCommandBusRoot temporary;
  const auto source = synthetic_ppm_source();
  auto project = ProjectStore::create(temporary.project(), source);
  CommandBus bus{project};

  const auto first_result =
      bus.execute(exposure_command(project, 0, "first", 1.0));
  REQUIRE(std::holds_alternative<CommitResult>(first_result));
  const auto committed = std::get<CommitResult>(first_result);

  const Command stale = exposure_command(project, 0, "stale", 0.25);
  const CommandExecutionResult stale_result = bus.execute(stale);

  REQUIRE(std::holds_alternative<CommandError>(stale_result));
  const CommandError& error = std::get<CommandError>(stale_result);
  CHECK(error.code == CommandErrorCode::RevisionConflict);
  CHECK(nps::core::to_string(error.code) == "REV_CONFLICT");
  CHECK_FALSE(error.project_modified);
  REQUIRE(error.current_revision.has_value());
  CHECK(*error.current_revision == 1);
  check_project_unchanged(project, 1, committed.snapshot, source);
}

TEST_CASE("CommandBus rejects an idempotency key reused for another payload") {
  TemporaryCommandBusRoot temporary;
  const auto source = synthetic_ppm_source();
  auto project = ProjectStore::create(temporary.project(), source);
  CommandBus bus{project};

  const Command first =
      exposure_command(project, 0, "shared-idempotency", 1.0);
  const auto first_result = bus.execute(first);
  REQUIRE(std::holds_alternative<CommitResult>(first_result));
  const auto committed = std::get<CommitResult>(first_result);

  Command reused = first;
  reused.command_id = "cmd-shared-idempotency-retry";
  reused.parameters = AdjustExposureParameters{.ev = 2.0};
  REQUIRE_FALSE(nps::core::has_same_idempotent_payload(first, reused));
  const CommandExecutionResult reused_result = bus.execute(reused);

  REQUIRE(std::holds_alternative<CommandError>(reused_result));
  const CommandError& error = std::get<CommandError>(reused_result);
  CHECK(error.code == CommandErrorCode::CmdIdempotencyReuse);
  CHECK(nps::core::to_string(error.code) == "CMD_IDEMPOTENCY_REUSE");
  CHECK_FALSE(error.project_modified);
  CHECK_FALSE(error.current_revision.has_value());
  check_project_unchanged(project, 1, committed.snapshot, source);
}

TEST_CASE("CommandBus maps undo and redo while revisions remain monotonic") {
  TemporaryCommandBusRoot temporary;
  const auto source = synthetic_ppm_source();
  auto project = ProjectStore::create(temporary.project(), source);
  CommandBus bus{project};

  const auto first_result =
      bus.execute(exposure_command(project, 0, "history-first", 1.0));
  REQUIRE(std::holds_alternative<CommitResult>(first_result));
  const auto first = std::get<CommitResult>(first_result);

  const auto second_result =
      bus.execute(exposure_command(project, 1, "history-second", 0.5));
  REQUIRE(std::holds_alternative<CommitResult>(second_result));
  const auto second = std::get<CommitResult>(second_result);
  CHECK(second.new_revision == 2);
  CHECK(second.snapshot.exposure_ev == 1.5);

  const auto undo_result = bus.execute(history_command(
      project, CommandKind::HistoryUndo, 2, "history-undo"));
  REQUIRE(std::holds_alternative<CommitResult>(undo_result));
  const auto undo = std::get<CommitResult>(undo_result);
  CHECK(undo.base_revision == 2);
  CHECK(undo.new_revision == 3);
  CHECK(undo.snapshot.id == first.snapshot.id);
  CHECK(undo.snapshot.exposure_ev == 1.0);

  const auto redo_result = bus.execute(history_command(
      project, CommandKind::HistoryRedo, 3, "history-redo"));
  REQUIRE(std::holds_alternative<CommitResult>(redo_result));
  const auto redo = std::get<CommitResult>(redo_result);
  CHECK(redo.base_revision == 3);
  CHECK(redo.new_revision == 4);
  CHECK(redo.snapshot.id == second.snapshot.id);
  CHECK(redo.snapshot.exposure_ev == 1.5);
  CHECK(project.current_revision() == 4);
  CHECK(project.read_source_bytes() == source);
}
