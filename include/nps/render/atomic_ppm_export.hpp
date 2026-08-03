#pragma once

#include "nps/color/color_encoding.hpp"
#include "nps/imaging/image_f32.hpp"
#include "nps/render/cancellation.hpp"

#include <cstdint>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace nps::render {

enum class ExistingFilePolicy {
  refuse_existing,
  replace_existing,
};

enum class AtomicPpmExportErrorCode {
  invalid_input,
  invalid_destination,
  unsafe_destination,
  forbidden_destination,
  destination_exists,
  cancelled,
  io_failure,
  verification_failed,
  publish_failed,
};

[[nodiscard]] std::string_view to_string(
    AtomicPpmExportErrorCode code) noexcept;

class AtomicPpmExportError final : public std::runtime_error {
 public:
  AtomicPpmExportError(
      AtomicPpmExportErrorCode code,
      std::string message);

  [[nodiscard]] AtomicPpmExportErrorCode code() const noexcept;

 private:
  AtomicPpmExportErrorCode code_;
};

// Deterministic fault injection for recovery tests. Production callers leave
// this at none.
enum class AtomicPpmExportFaultPoint {
  none,
  after_first_write,
  hard_exit_after_flush,
};

inline constexpr int kAtomicPpmExportHardExitCode = 86;

// Carries the trusted persisted document state that produced an export. The
// document and snapshot IDs use the project's stable ASCII identifier grammar.
// Hashes are lowercase SHA-256 values. source_hash identifies the persisted
// source asset (not the rendered ImageF32), and graph_hash identifies the edit
// graph. color_id must be supported and must match ImageF32::encoding.
//
// This layer validates the identity's shape and color consistency; callers
// must obtain the values from the authoritative project snapshot rather than
// accepting untrusted metadata.
struct AtomicPpmExportIdentity final {
  std::string document_id;
  std::string snapshot_id;
  std::int64_t revision{};
  std::string source_hash;
  std::string graph_hash;
  std::string color_id;
};

// Exports a strict P6, 16-bit, big-endian PPM. ImageF32 is premultiplied RGBA,
// so callers must always provide an explicit opaque conversion/matte policy.
//
// A random, exclusively-created ordinary file in the destination directory is
// flushed to stable storage, closed, strictly decoded, and compared pixel for
// pixel before publication. All pre-publication failures and cancellation
// leave an existing destination untouched and remove the owned temporary file.
// An injected or real process termination cannot run that cleanup and may
// leave a private `.nps-export-*.tmp` sibling; it never exposes that file under
// the requested `.ppm` name. Startup orphan cleanup is outside the M1 API.
//
// Publication uses a same-directory atomic rename where the platform provides
// one (MoveFileExW on Windows, rename/renameat2/renamex_np on
// supported POSIX systems). Atomicity and durability ultimately retain the
// guarantees of the destination filesystem. A process or machine failure after
// publication but before the directory entry reaches stable storage may expose
// either the complete old file or the complete new file, never a partial file.
//
// forbidden_paths is mandatory and must contain at least one protected project
// path selected from the authoritative project safety scope. Every entry must
// resolve to an existing filesystem object during request validation; missing
// or unresolvable entries reject the request as privacy-safe invalid input. A
// destination equal to or below any protected path is rejected. Existing paths
// also use filesystem-equivalence checks so case aliases, short names, and hard
// links cannot bypass the lexical containment check.
//
// On POSIX, the existing destination parent is canonicalized before temporary
// creation and publication. This safely follows system aliases such as macOS
// /var -> /private/var while an existing destination symlink remains rejected.
// Windows M1 exports are limited to local volumes: ordinary network paths and
// mapped remote drives are rejected. Windows also rejects reparse points,
// alternate data streams, DOS device aliases, and components ending in a dot
// or space. These checks reduce accidental project-source overwrite but are not
// a sandbox: an attacker who can replace path components or the private
// temporary directory entry concurrently still creates a documented TOCTOU
// boundary between verification and publication.
void export_atomic_ppm16(
    const imaging::ImageF32& image,
    const AtomicPpmExportIdentity& identity,
    const std::filesystem::path& destination,
    const color::OpaqueImage16Options& opaque_options,
    std::span<const std::filesystem::path> forbidden_paths,
    ExistingFilePolicy existing_file_policy =
        ExistingFilePolicy::refuse_existing,
    CancellationToken cancellation = {},
    AtomicPpmExportFaultPoint fault_point =
        AtomicPpmExportFaultPoint::none);

}  // namespace nps::render
