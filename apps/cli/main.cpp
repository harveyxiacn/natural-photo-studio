#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

#include "nps/core/command.hpp"
#include "nps/core/command_bus.hpp"
#include "nps/core/project_store.hpp"
#include "nps/imaging/operations.hpp"
#include "nps/imaging/ppm16.hpp"
#include "nps/imaging/synthetic.hpp"

namespace {

using nps::core::Command;
using nps::core::CommandBus;
using nps::core::CommandError;
using nps::core::CommandExecutionResult;
using nps::core::CommandKind;
using nps::core::CommitResult;
using nps::core::FaultPoint;
using nps::core::ProjectStore;
using nps::core::ProjectStoreError;
using nps::imaging::Image16;

[[nodiscard]] std::span<const std::uint8_t> as_unsigned_bytes(
    const std::vector<std::byte>& bytes) {
  return {
      reinterpret_cast<const std::uint8_t*>(bytes.data()),
      bytes.size()};
}

[[nodiscard]] std::span<const std::byte> as_bytes(
    const std::vector<std::uint8_t>& bytes) {
  return {
      reinterpret_cast<const std::byte*>(bytes.data()),
      bytes.size()};
}

[[nodiscard]] Command strict_round_trip(Command command) {
  const std::string encoded = nps::core::command_to_json(command).dump();
  auto parsed = nps::core::parse_command_json(encoded);
  if (const auto* error = std::get_if<CommandError>(&parsed)) {
    throw std::runtime_error(error->message);
  }
  return std::get<Command>(std::move(parsed));
}

[[nodiscard]] Command make_exposure_command(
    std::string command_id,
    std::string idempotency_key,
    std::string document_id,
    std::uint64_t expected_revision,
    double exposure_ev) {
  return strict_round_trip(Command{
      .command_id = std::move(command_id),
      .idempotency_key = std::move(idempotency_key),
      .document_id = std::move(document_id),
      .expected_revision = expected_revision,
      .kind = CommandKind::AdjustExposure,
      .parameters =
          nps::core::AdjustExposureParameters{.ev = exposure_ev},
      .privacy = {}});
}

[[nodiscard]] CommitResult require_commit(
    CommandExecutionResult result) {
  if (const auto* error = std::get_if<CommandError>(&result)) {
    throw std::runtime_error(
        std::string(nps::core::to_string(error->code)) + ": " +
        error->message);
  }
  return std::get<CommitResult>(std::move(result));
}

[[nodiscard]] Image16 render_current(ProjectStore& project) {
  const auto source_bytes = project.read_source_bytes();
  const Image16 source = nps::imaging::decode_ppm16(as_bytes(source_bytes));
  return nps::imaging::apply_exposure(
      source, project.current_snapshot().exposure_ev);
}

[[nodiscard]] nlohmann::json result_json(
    const CommitResult& result) {
  return {
      {"status", "committed"},
      {"commandId", result.command_id},
      {"baseRevision", result.base_revision},
      {"newRevision", result.new_revision},
      {"snapshotId", result.snapshot.id},
      {"sourceHash", result.snapshot.source_hash},
      {"exposureEv", result.snapshot.exposure_ev},
      {"idempotentReplay", result.idempotent_replay}};
}

int run_demo(const std::filesystem::path& project_path) {
  const Image16 source =
      nps::imaging::make_deterministic_gradient(32, 24);
  const auto encoded_source = nps::imaging::encode_ppm16(source);

  ProjectStore project =
      ProjectStore::create(project_path, as_unsigned_bytes(encoded_source));
  CommandBus bus(project);
  const Command exposure =
      make_exposure_command(
          "demo-exposure-001",
          "demo/session:1",
          project.document_id(),
          0,
          1.0);
  const CommitResult commit = require_commit(bus.execute(exposure));
  const Image16 first_render = render_current(project);
  nps::imaging::write_ppm16_file(
      project_path / "previews" / "current.ppm", first_render);
  const auto first_bytes = nps::imaging::encode_ppm16(first_render);
  project.close();

  ProjectStore reopened = ProjectStore::open(project_path);
  const Image16 reopened_render = render_current(reopened);
  const auto reopened_bytes = nps::imaging::encode_ppm16(reopened_render);
  const auto integrity = reopened.verify_integrity();
  if (first_bytes != reopened_bytes) {
    throw std::runtime_error(
        "Reopening the project changed the rendered pixels.");
  }

  nlohmann::json output = result_json(commit);
  output["projectFormat"] = "nps.project/v1";
  output["renderByteIdenticalAfterReopen"] = true;
  output["referencedObjects"] = integrity.referenced_objects;
  output["orphanObjects"] = integrity.orphan_objects;
  std::cout << output.dump(2) << '\n';
  return 0;
}

int run_verify(const std::filesystem::path& project_path) {
  ProjectStore project = ProjectStore::open(project_path);
  const auto integrity = project.verify_integrity();
  const auto first = nps::imaging::encode_ppm16(render_current(project));
  const auto second = nps::imaging::encode_ppm16(render_current(project));
  if (first != second) {
    throw std::runtime_error("The CPU reference renderer is not deterministic.");
  }

  const auto snapshot = project.current_snapshot();
  std::cout
      << nlohmann::json{
             {"status", "verified"},
             {"revision", integrity.revision},
             {"snapshotId", snapshot.id},
             {"sourceHash", snapshot.source_hash},
             {"exposureEv", snapshot.exposure_ev},
             {"referencedObjects", integrity.referenced_objects},
             {"orphanObjects", integrity.orphan_objects},
             {"recoveredUncleanShutdown",
              integrity.recovered_unclean_shutdown},
             {"deterministicRender", true}}
             .dump(2)
      << '\n';
  return 0;
}

int run_crash_commit(const std::filesystem::path& project_path) {
  ProjectStore project = ProjectStore::open(project_path);
  const std::uint64_t revision =
      static_cast<std::uint64_t>(project.current_revision());
  const Command command = make_exposure_command(
      "recovery-exposure-001",
      "recovery/session:1",
      project.document_id(),
      revision,
      0.5);
  CommandBus bus(project);
  static_cast<void>(
      bus.execute(command, FaultPoint::after_database_committed));
  throw std::runtime_error("The crash fault point was not reached.");
}

int run_crash_create_after_object(
    const std::filesystem::path& project_path) {
  const Image16 source =
      nps::imaging::make_deterministic_gradient(16, 12);
  const auto encoded_source = nps::imaging::encode_ppm16(source);
  static_cast<void>(ProjectStore::create(
      project_path,
      as_unsigned_bytes(encoded_source),
      FaultPoint::after_object_persisted));
  throw std::runtime_error("The create crash fault point was not reached.");
}

int run_verify_recovery(const std::filesystem::path& project_path) {
  ProjectStore project = ProjectStore::open(project_path);
  const auto integrity = project.verify_integrity();
  if (!integrity.recovered_unclean_shutdown ||
      integrity.revision < 1) {
    throw std::runtime_error("The expected unclean shutdown was not recovered.");
  }

  const std::uint64_t original_base =
      static_cast<std::uint64_t>(integrity.revision - 1);
  const Command retry = make_exposure_command(
      "recovery-exposure-retry-002",
      "recovery/session:1",
      project.document_id(),
      original_base,
      0.5);
  CommandBus bus(project);
  const CommitResult result = require_commit(bus.execute(retry));
  if (!result.idempotent_replay ||
      result.new_revision != integrity.revision) {
    throw std::runtime_error(
        "The recovered transaction was not replayed idempotently.");
  }
  static_cast<void>(render_current(project));

  nlohmann::json output = result_json(result);
  output["status"] = "recovered";
  output["recoveredUncleanShutdown"] = true;
  output["committedTransactionPreserved"] = true;
  std::cout << output.dump(2) << '\n';
  return 0;
}

void print_usage() {
  std::cerr
      << "Usage:\n"
      << "  nps-cli demo <project.npsproj>\n"
      << "  nps-cli verify <project.npsproj>\n"
      << "  nps-cli crash-create-after-object <project.npsproj>\n"
      << "  nps-cli crash-commit <project.npsproj>\n"
      << "  nps-cli verify-recovery <project.npsproj>\n";
}

}  // namespace

int main(int argument_count, char** arguments) {
  if (argument_count != 3) {
    print_usage();
    return 64;
  }

  try {
    const std::string_view operation(arguments[1]);
    const std::filesystem::path project_path(arguments[2]);
    if (operation == "demo") {
      return run_demo(project_path);
    }
    if (operation == "verify") {
      return run_verify(project_path);
    }
    if (operation == "crash-commit") {
      return run_crash_commit(project_path);
    }
    if (operation == "crash-create-after-object") {
      return run_crash_create_after_object(project_path);
    }
    if (operation == "verify-recovery") {
      return run_verify_recovery(project_path);
    }
    print_usage();
    return 64;
  } catch (const ProjectStoreError& error) {
    std::cerr
        << nlohmann::json{
               {"status", "failed"},
               {"code", error.code()},
               {"message", error.what()},
               {"projectModified", false}}
               .dump()
        << '\n';
    return 2;
  } catch (const std::exception& error) {
    std::cerr
        << nlohmann::json{
               {"status", "failed"},
               {"code", "INTERNAL_ERROR"},
               {"message", error.what()}}
               .dump()
        << '\n';
    return 1;
  }
}
