#include "nps/core/command_bus.hpp"

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>

namespace nps::core {
namespace {

[[nodiscard]] CommandError schema_error(std::string message) {
  return CommandError{
      .code = CommandErrorCode::CmdSchemaInvalid,
      .message = std::move(message),
      .project_modified = false,
      .current_revision = std::nullopt};
}

}  // namespace

CommandBus::CommandBus(ProjectStore& project) noexcept : project_(&project) {}

CommandExecutionResult CommandBus::execute(
    const Command& command,
    FaultPoint fault_point) const {
  if (const auto validation_error = validate_command(command)) {
    return *validation_error;
  }
  if (command.document_id != project_->document_id()) {
    return schema_error("The command targets a different document.");
  }
  if (command.expected_revision >
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    return schema_error("The expected revision is outside the storage range.");
  }

  StoreCommand store_command{
      .command_id = command.command_id,
      .idempotency_key = command.idempotency_key,
      .request_fingerprint = canonical_idempotency_payload(command),
      .expected_revision =
          static_cast<std::int64_t>(command.expected_revision),
      .mutation = StoreMutation::adjust_exposure,
      .exposure_delta_ev = 0.0};

  switch (command.kind) {
    case CommandKind::AdjustExposure:
      store_command.mutation = StoreMutation::adjust_exposure;
      store_command.exposure_delta_ev =
          std::get<AdjustExposureParameters>(command.parameters).ev;
      break;
    case CommandKind::HistoryUndo:
      store_command.mutation = StoreMutation::undo;
      break;
    case CommandKind::HistoryRedo:
      store_command.mutation = StoreMutation::redo;
      break;
  }

  try {
    return project_->execute(store_command, fault_point);
  } catch (const ProjectStoreError& error) {
    if (error.code() == "CMD_IDEMPOTENCY_REUSE") {
      return make_idempotency_reuse_error(command.idempotency_key);
    }
    if (error.code() == "REV_CONFLICT") {
      return make_revision_conflict_error(
          command.expected_revision,
          static_cast<std::uint64_t>(project_->current_revision()));
    }
    throw;
  }
}

}  // namespace nps::core
