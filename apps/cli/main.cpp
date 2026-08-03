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

#include "nps/color/color_encoding.hpp"
#include "nps/core/command.hpp"
#include "nps/core/command_bus.hpp"
#include "nps/core/project_store.hpp"
#include "nps/imaging/operations.hpp"
#include "nps/imaging/ppm16.hpp"
#include "nps/imaging/synthetic.hpp"
#include "nps/render/atomic_ppm_export.hpp"
#include "nps/render/cpu_renderer.hpp"

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
using nps::imaging::ImageF32;

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

[[nodiscard]] nps::color::OpaqueImage16Options
reference_export_options() {
  return {
      .destination_encoding =
          nps::color::ColorEncoding::scene_linear_rec2020_d65,
      .matte_rgb = {0.0F, 0.0F, 0.0F}};
}

[[nodiscard]] std::filesystem::path absolute_project_root(
    const std::filesystem::path& project_path) {
  std::error_code error;
  const auto absolute = std::filesystem::absolute(project_path, error);
  if (error || absolute.empty()) {
    throw ProjectStoreError(
        "IO_PROJECT_PATH",
        "The project path could not be normalized.");
  }
  return absolute.lexically_normal();
}

[[nodiscard]] std::vector<std::filesystem::path>
protected_project_paths(
    const std::filesystem::path& project_root,
    const nps::core::Snapshot& snapshot) {
  if (snapshot.source_hash.size() != 64U) {
    throw ProjectStoreError(
        "IO_PROJECT_FORMAT",
        "The current snapshot source hash is invalid.");
  }

  const auto database = project_root / "project.db";
  const auto objects = project_root / "objects";
  const auto source_object =
      objects / "sha256" / snapshot.source_hash.substr(0, 2) /
      snapshot.source_hash.substr(2);
  return {
      project_root,
      database,
      objects,
      source_object,
  };
}

int run_project_export(
    const std::filesystem::path& project_path,
    const std::filesystem::path& destination,
    nps::render::ExistingFilePolicy existing_file_policy,
    nps::render::AtomicPpmExportFaultPoint fault_point =
        nps::render::AtomicPpmExportFaultPoint::none) {
  ProjectStore project = ProjectStore::open(project_path);
  if (project.project_format() != "nps.project/v2") {
    throw ProjectStoreError(
        "IO_PROJECT_MIGRATION_REQUIRED",
        "Reference export requires an explicitly migrated v2 project.");
  }
  static_cast<void>(project.verify_integrity());

  const auto snapshot = project.current_snapshot();
  auto parsed_graph =
      nps::document::parse_edit_graph_json(snapshot.edit_graph_json);
  if (const auto* error =
          std::get_if<nps::document::EditGraphError>(&parsed_graph)) {
    throw ProjectStoreError(
        "IO_PROJECT_FORMAT",
        std::string("The current edit graph is invalid: ") +
            std::string(nps::document::to_string(error->code)));
  }
  const auto graph =
      std::get<nps::document::EditGraph>(std::move(parsed_graph));

  const auto source_bytes = project.read_source_bytes();
  const Image16 source_image16 =
      nps::imaging::decode_ppm16(as_bytes(source_bytes));
  constexpr auto encoding =
      nps::color::ColorEncoding::scene_linear_rec2020_d65;
  const ImageF32 source_image = nps::color::image16_to_opaque_f32(
      source_image16, encoding, encoding);
  const std::int64_t revision = project.current_revision();
  const std::string snapshot_id =
      "snapshot-" + std::to_string(snapshot.id);
  const nps::render::RenderRequest request{
      .document_id = project.document_id(),
      .snapshot_id = snapshot_id,
      .revision = revision,
      .source_hash = nps::imaging::image_f32_sha256(source_image),
      .generation = 1U,
      .roi =
          {
              .x = 0U,
              .y = 0U,
              .width = source_image.width,
              .height = source_image.height,
          },
      .quality = nps::render::RenderQuality::final,
      .tile_size = 512U,
      .worker_count = 1U,
  };
  const auto rendered = nps::render::CpuRenderer{}.render(
      source_image,
      graph,
      std::span<const nps::render::MaskAssetView>{},
      request);

  const nps::render::AtomicPpmExportIdentity identity{
      .document_id = project.document_id(),
      .snapshot_id = snapshot_id,
      .revision = revision,
      .source_hash = snapshot.source_hash,
      .graph_hash = snapshot.edit_graph_sha256,
      .color_id = snapshot.working_color_id,
  };
  const auto forbidden =
      protected_project_paths(absolute_project_root(project_path), snapshot);
  nps::render::export_atomic_ppm16(
      rendered.image,
      identity,
      destination,
      reference_export_options(),
      forbidden,
      existing_file_policy,
      {},
      fault_point);
  std::cout
      << nlohmann::json{
             {"status", "exported"},
             {"format", "P6-PPM-16BE"},
             {"width", rendered.image.width},
             {"height", rendered.image.height},
             {"colorSpace",
              nps::color::scene_linear_rec2020_d65_id}}
             .dump(2)
      << '\n';
  return 0;
}

int run_export_demo(
    const std::filesystem::path& project_path,
    const std::filesystem::path& destination) {
  return run_project_export(
      project_path,
      destination,
      nps::render::ExistingFilePolicy::refuse_existing);
}

int run_crash_export_after_flush(
    const std::filesystem::path& project_path,
    const std::filesystem::path& destination) {
  return run_project_export(
      project_path,
      destination,
      nps::render::ExistingFilePolicy::refuse_existing,
      nps::render::AtomicPpmExportFaultPoint::hard_exit_after_flush);
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

int run_migrate(
    const std::filesystem::path& source_path,
    const std::filesystem::path& target_path,
    FaultPoint fault_point = FaultPoint::none) {
  ProjectStore migrated = ProjectStore::migrate_v1_to_v2(
      source_path, target_path, fault_point);
  const auto integrity = migrated.verify_integrity();
  std::cout
      << nlohmann::json{
             {"status", "migrated"},
             {"projectFormat", migrated.project_format()},
             {"documentId", migrated.document_id()},
             {"revision", integrity.revision},
             {"snapshotId", migrated.current_snapshot().id}}
             .dump(2)
      << '\n';
  return 0;
}

void print_usage() {
  std::cerr
      << "Usage:\n"
      << "  nps-cli demo <project.npsproj>\n"
      << "  nps-cli verify <project.npsproj>\n"
      << "  nps-cli crash-create-after-object <project.npsproj>\n"
      << "  nps-cli crash-commit <project.npsproj>\n"
      << "  nps-cli verify-recovery <project.npsproj>\n"
      << "  nps-cli export-demo <v2-project.npsproj> <output.ppm>\n"
      << "  nps-cli crash-export-after-flush "
         "<v2-project.npsproj> <output.ppm>\n"
      << "  nps-cli migrate-v1-v2 <source.npsproj> <target.npsproj>\n"
      << "  nps-cli crash-migrate-staged <source.npsproj> <target.npsproj>\n"
      << "  nps-cli crash-migrate-published <source.npsproj> <target.npsproj>\n";
}

}  // namespace

int main(int argument_count, char** arguments) {
  if (argument_count != 3 && argument_count != 4) {
    print_usage();
    return 64;
  }

  try {
    const std::string_view operation(arguments[1]);
    if (argument_count == 4) {
      const std::filesystem::path source_path(arguments[2]);
      const std::filesystem::path target_path(arguments[3]);
      if (operation == "export-demo") {
        return run_export_demo(source_path, target_path);
      }
      if (operation == "crash-export-after-flush") {
        return run_crash_export_after_flush(source_path, target_path);
      }
      if (operation == "migrate-v1-v2") {
        return run_migrate(source_path, target_path);
      }
      if (operation == "crash-migrate-staged") {
        return run_migrate(
            source_path,
            target_path,
            FaultPoint::after_migration_staged);
      }
      if (operation == "crash-migrate-published") {
        return run_migrate(
            source_path,
            target_path,
            FaultPoint::after_migration_published);
      }
      print_usage();
      return 64;
    }
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
  } catch (const nps::render::AtomicPpmExportError& error) {
    std::cerr
        << nlohmann::json{
               {"status", "failed"},
               {"code",
                std::string("EXPORT_") +
                    std::string(nps::render::to_string(error.code()))},
               {"message", error.what()},
               {"projectModified", false}}
               .dump()
        << '\n';
    return 3;
  } catch (const nps::render::RenderError& error) {
    std::cerr
        << nlohmann::json{
               {"status", "failed"},
               {"code",
                std::string("RENDER_") +
                    std::string(nps::render::to_string(error.code()))},
               {"message", error.what()},
               {"projectModified", false}}
               .dump()
        << '\n';
    return 4;
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
