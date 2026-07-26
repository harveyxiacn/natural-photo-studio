#include "nps/core/project_store.hpp"
#include "nps/imaging/ppm16.hpp"
#include "nps/imaging/synthetic.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using nps::core::ProjectStore;
using nps::core::ProjectStoreError;
using nps::core::StoreCommand;
using nps::core::StoreMutation;

class TemporaryProjectRoot final {
 public:
  TemporaryProjectRoot() {
    static std::atomic_uint64_t sequence{};
    const auto timestamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("nps-project-store-" + std::to_string(timestamp) + "-" +
             std::to_string(sequence.fetch_add(std::uint64_t{1})));
    if (!std::filesystem::create_directory(path_)) {
      throw std::runtime_error{"unable to create project-store test directory"};
    }
  }

  ~TemporaryProjectRoot() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  TemporaryProjectRoot(const TemporaryProjectRoot&) = delete;
  TemporaryProjectRoot& operator=(const TemporaryProjectRoot&) = delete;

  [[nodiscard]] std::filesystem::path project(
      const std::filesystem::path& filename =
          std::filesystem::path{"test.npsproj"}) const {
    return path_ / filename;
  }

 private:
  std::filesystem::path path_;
};

[[nodiscard]] std::vector<std::uint8_t> as_unsigned_bytes(
    const std::vector<std::byte>& bytes) {
  std::vector<std::uint8_t> result;
  result.reserve(bytes.size());
  for (const std::byte value : bytes) {
    result.push_back(std::to_integer<std::uint8_t>(value));
  }
  return result;
}

[[nodiscard]] std::vector<std::uint8_t> make_source_bytes(
    std::uint32_t width = 19,
    std::uint32_t height = 11) {
  return as_unsigned_bytes(nps::imaging::encode_ppm16(
      nps::imaging::make_deterministic_gradient(width, height)));
}

[[nodiscard]] StoreCommand command(
    StoreMutation mutation,
    std::int64_t expected_revision,
    std::string token,
    double exposure_delta_ev = 0.0) {
  return StoreCommand{
      .command_id = "cmd-" + token,
      .idempotency_key = "idempotency-" + token,
      .request_fingerprint = "fingerprint-" + token,
      .expected_revision = expected_revision,
      .mutation = mutation,
      .exposure_delta_ev = exposure_delta_ev};
}

template <typename Action>
void require_store_error(std::string_view expected_code, Action&& action) {
  try {
    std::forward<Action>(action)();
    FAIL("Expected ProjectStoreError with code " << expected_code);
  } catch (const ProjectStoreError& error) {
    CHECK(error.code() == expected_code);
  }
}

[[nodiscard]] bool is_lower_sha256(std::string_view value) {
  if (value.size() != 64U) {
    return false;
  }
  for (const char character : value) {
    const bool digit = character >= '0' && character <= '9';
    const bool lower_hex = character >= 'a' && character <= 'f';
    if (!digit && !lower_hex) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] std::filesystem::path object_path(
    const std::filesystem::path& project,
    std::string_view hash) {
  return project / "objects" / "sha256" /
         std::string{hash.substr(0, 2)} / std::string{hash.substr(2)};
}

}  // namespace

TEST_CASE("project creation is content-addressed and preserves source bytes") {
  TemporaryProjectRoot temporary;
  const auto project_path = temporary.project();
  const auto duplicate_path = temporary.project("duplicate.npsproj");
  const auto different_path = temporary.project("different.npsproj");

  const auto generated = nps::imaging::make_deterministic_gradient(19, 11);
  const auto generated_before_create = generated;
  const auto source =
      as_unsigned_bytes(nps::imaging::encode_ppm16(generated));
  const auto source_before_create = source;
  const auto different_source = make_source_bytes(17, 13);

  auto project = ProjectStore::create(project_path, source);
  auto duplicate = ProjectStore::create(duplicate_path, source);
  auto different = ProjectStore::create(different_path, different_source);

  CHECK_FALSE(project.document_id().empty());
  CHECK(project.current_revision() == 0);
  const auto initial = project.current_snapshot();
  CHECK(initial.created_revision == 0);
  CHECK(initial.exposure_ev == 0.0);
  REQUIRE(is_lower_sha256(initial.source_hash));
  CHECK(project.read_source_bytes() == source);
  CHECK(generated == generated_before_create);
  CHECK(source == source_before_create);

  CHECK(project.document_id() != duplicate.document_id());
  CHECK(project.document_id() != different.document_id());
  CHECK(duplicate.current_snapshot().source_hash == initial.source_hash);
  CHECK(different.current_snapshot().source_hash != initial.source_hash);
  CHECK(std::filesystem::is_regular_file(
      object_path(project_path, initial.source_hash)));
  CHECK_FALSE(
      std::filesystem::exists(project_path / ".nps-creating"));
  for (const auto& entry :
       std::filesystem::directory_iterator(project_path.parent_path())) {
    const auto filename = entry.path().filename().u8string();
    CHECK_FALSE(filename.starts_with(u8".nps-creating-"));
  }

  const auto integrity = project.verify_integrity();
  CHECK(integrity.revision == 0);
  CHECK(integrity.referenced_objects == 1);
  CHECK(integrity.orphan_objects == 0);
  CHECK_FALSE(integrity.recovered_unclean_shutdown);
}

TEST_CASE("create never overwrites an existing project") {
  TemporaryProjectRoot temporary;
  const auto project_path = temporary.project();
  const auto source = make_source_bytes();
  auto project = ProjectStore::create(project_path, source);
  const auto document_id = project.document_id();
  const auto source_hash = project.current_snapshot().source_hash;

  require_store_error("IO_PROJECT_EXISTS", [&] {
    static_cast<void>(ProjectStore::create(
        project_path, make_source_bytes(7, 5)));
  });

  CHECK(project.document_id() == document_id);
  CHECK(project.current_snapshot().source_hash == source_hash);
  CHECK(project.read_source_bytes() == source);
  CHECK(project.verify_integrity().orphan_objects == 0);
}

TEST_CASE("exposure commits enforce revisions and idempotency") {
  TemporaryProjectRoot temporary;
  const auto project_path = temporary.project();
  const auto source = make_source_bytes();
  auto project = ProjectStore::create(project_path, source);
  const auto initial = project.current_snapshot();

  const auto exposure =
      command(StoreMutation::adjust_exposure, 0, "exposure-1", 1.0);
  const auto committed = project.execute(exposure);
  CHECK(committed.command_id == exposure.command_id);
  CHECK(committed.base_revision == 0);
  CHECK(committed.new_revision == 1);
  CHECK_FALSE(committed.idempotent_replay);
  CHECK(committed.snapshot.created_revision == 1);
  CHECK(committed.snapshot.source_hash == initial.source_hash);
  CHECK(committed.snapshot.exposure_ev == 1.0);
  CHECK(project.current_revision() == 1);
  CHECK(project.read_source_bytes() == source);

  auto retry = exposure;
  retry.command_id = "cmd-exposure-1-transport-retry";
  const auto replayed = project.execute(retry);
  CHECK(replayed.command_id == committed.command_id);
  CHECK(replayed.base_revision == committed.base_revision);
  CHECK(replayed.new_revision == committed.new_revision);
  CHECK(replayed.snapshot.id == committed.snapshot.id);
  CHECK(replayed.idempotent_replay);
  CHECK(project.current_revision() == 1);

  auto reused = exposure;
  reused.command_id = "cmd-idempotency-reuse";
  reused.request_fingerprint = "fingerprint-different-payload";
  reused.exposure_delta_ev = 2.0;
  require_store_error("CMD_IDEMPOTENCY_REUSE", [&] {
    static_cast<void>(project.execute(reused));
  });
  CHECK(project.current_revision() == 1);

  const auto stale =
      command(StoreMutation::adjust_exposure, 0, "stale", 0.25);
  require_store_error("REV_CONFLICT", [&] {
    static_cast<void>(project.execute(stale));
  });
  CHECK(project.current_revision() == 1);
  CHECK(project.current_snapshot().id == committed.snapshot.id);
  CHECK(project.read_source_bytes() == source);

  const auto integrity = project.verify_integrity();
  CHECK(integrity.revision == 1);
  CHECK(integrity.referenced_objects == 1);
  CHECK(integrity.orphan_objects == 0);
}

TEST_CASE("undo redo and branching keep revisions monotonic") {
  TemporaryProjectRoot temporary;
  const auto source = make_source_bytes();
  auto project = ProjectStore::create(temporary.project(), source);

  const auto first = project.execute(
      command(StoreMutation::adjust_exposure, 0, "first", 1.0));
  const auto second = project.execute(
      command(StoreMutation::adjust_exposure, 1, "second", 0.5));
  CHECK(first.new_revision == 1);
  CHECK(second.new_revision == 2);
  CHECK(second.snapshot.exposure_ev == 1.5);

  const auto undo =
      project.execute(command(StoreMutation::undo, 2, "undo-1"));
  CHECK(undo.base_revision == 2);
  CHECK(undo.new_revision == 3);
  CHECK(undo.snapshot.id == first.snapshot.id);
  CHECK(undo.snapshot.exposure_ev == 1.0);
  CHECK(project.current_revision() == 3);

  const auto redo =
      project.execute(command(StoreMutation::redo, 3, "redo-1"));
  CHECK(redo.base_revision == 3);
  CHECK(redo.new_revision == 4);
  CHECK(redo.snapshot.id == second.snapshot.id);
  CHECK(redo.snapshot.exposure_ev == 1.5);

  const auto branch_base =
      project.execute(command(StoreMutation::undo, 4, "undo-for-branch"));
  CHECK(branch_base.new_revision == 5);
  CHECK(branch_base.snapshot.id == first.snapshot.id);

  const auto branch = project.execute(
      command(StoreMutation::adjust_exposure, 5, "branch", -0.25));
  CHECK(branch.base_revision == 5);
  CHECK(branch.new_revision == 6);
  CHECK(branch.snapshot.created_revision == 6);
  CHECK(branch.snapshot.id != second.snapshot.id);
  CHECK(branch.snapshot.exposure_ev == 0.75);
  CHECK(project.current_revision() == 6);

  const auto invalid_redo =
      command(StoreMutation::redo, 6, "redo-past-branch");
  require_store_error("CMD_HISTORY_BOUNDARY", [&] {
    static_cast<void>(project.execute(invalid_redo));
  });
  CHECK(project.current_revision() == 6);
  CHECK(project.current_snapshot().id == branch.snapshot.id);
  CHECK(project.read_source_bytes() == source);

  const auto integrity = project.verify_integrity();
  CHECK(integrity.revision == 6);
  CHECK(integrity.referenced_objects == 1);
  CHECK(integrity.orphan_objects == 0);
}

TEST_CASE("clean close and reopen retain the committed project") {
  TemporaryProjectRoot temporary;
  const auto project_path =
      temporary.project(std::filesystem::path{u8"自然照片.npsproj"});
  const auto source = make_source_bytes();
  auto project = ProjectStore::create(project_path, source);
  const auto committed = project.execute(
      command(StoreMutation::adjust_exposure, 0, "persisted", 1.0));

  project.close();
  require_store_error("IO_PROJECT_CLOSED", [&] {
    static_cast<void>(project.current_revision());
  });

  auto reopened = ProjectStore::open(project_path);
  CHECK(reopened.current_revision() == committed.new_revision);
  CHECK(reopened.current_snapshot().id == committed.snapshot.id);
  CHECK(reopened.current_snapshot().exposure_ev == 1.0);
  CHECK(reopened.read_source_bytes() == source);

  const auto integrity = reopened.verify_integrity();
  CHECK(integrity.revision == committed.new_revision);
  CHECK(integrity.referenced_objects == 1);
  CHECK(integrity.orphan_objects == 0);
  CHECK_FALSE(integrity.recovered_unclean_shutdown);
}

TEST_CASE("an open project holds an exclusive project lease") {
  TemporaryProjectRoot temporary;
  const auto project_path = temporary.project();
  auto first =
      ProjectStore::create(project_path, make_source_bytes());

  require_store_error("IO_PROJECT_LOCKED", [&] {
    auto competing = ProjectStore::open(project_path);
    static_cast<void>(competing);
  });

  first.close();
  auto reopened = ProjectStore::open(project_path);
  CHECK(reopened.current_revision() == 0);
}

TEST_CASE("integrity reports orphan objects and rejects source corruption") {
  TemporaryProjectRoot temporary;
  const auto project_path = temporary.project();
  const auto source = make_source_bytes();
  auto project = ProjectStore::create(project_path, source);
  const auto source_object =
      object_path(project_path, project.current_snapshot().source_hash);

  const auto donor_path = temporary.project("orphan-donor.npsproj");
  auto donor = ProjectStore::create(
      donor_path, make_source_bytes(23, 7));
  const auto donor_hash = donor.current_snapshot().source_hash;
  REQUIRE(donor_hash != project.current_snapshot().source_hash);
  const auto donor_object = object_path(donor_path, donor_hash);
  const auto orphan = object_path(project_path, donor_hash);
  std::error_code directory_error;
  std::filesystem::create_directories(
      orphan.parent_path(), directory_error);
  REQUIRE_FALSE(directory_error);
  std::error_code copy_error;
  REQUIRE(std::filesystem::copy_file(
      donor_object, orphan, std::filesystem::copy_options::none, copy_error));
  REQUIRE_FALSE(copy_error);

  const auto with_orphan = project.verify_integrity();
  CHECK(with_orphan.referenced_objects == 1);
  CHECK(with_orphan.orphan_objects == 1);

  {
    std::ofstream stream{source_object, std::ios::binary | std::ios::trunc};
    REQUIRE(stream);
    stream.put('\0');
    REQUIRE(stream);
  }
  require_store_error("IO_OBJECT_CORRUPT", [&] {
    static_cast<void>(project.verify_integrity());
  });
}

TEST_CASE("integrity rejects a linked object-store root") {
  TemporaryProjectRoot temporary;
  const auto project_path = temporary.project();
  auto project =
      ProjectStore::create(project_path, make_source_bytes());

  const auto object_root = project_path / "objects" / "sha256";
  const auto real_object_root =
      project_path / "objects" / "sha256-real";
  std::error_code rename_error;
  std::filesystem::rename(
      object_root, real_object_root, rename_error);
  REQUIRE_FALSE(rename_error);

  std::error_code link_error;
  std::filesystem::create_directory_symlink(
      real_object_root, object_root, link_error);
  if (link_error) {
    WARN(
        "Directory symlinks are unavailable in this test environment: "
        << link_error.message());
    return;
  }

  require_store_error("IO_OBJECT_LINK", [&] {
    static_cast<void>(project.verify_integrity());
  });
}
