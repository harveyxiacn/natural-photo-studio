# Changelog

All notable changes will be documented here. The project follows semantic versioning once public API compatibility begins.

## [Unreleased]

### Added

- Public-safe repository policy, contribution and security documentation.
- Executable Milestone 0 reference core for commands, non-destructive edits, project persistence, rendering, and recovery verification.
- Cross-platform CI, CodeQL, documentation checks, and repository privacy scanning.
- Windows and macOS desktop packaging, GPU, signing, and notarization targets.
- Pinned Git-history secret scanning and stricter fixture-license, local-path, project-file, and ignored-directory privacy gates.
- Explicit FP32 premultiplied-RGBA and stable color-encoding contracts.
- Immutable, canonical `nps.edit-graph/v1` DAGs with exposure, RGB curves,
  content-addressed `Mask16` bindings, and strict `graph.replace` commands.
- A deterministic CPU reference renderer with ROI/tile scheduling, bounded
  transactional caching, cancellation, and stale-result publication gates.
- Explicit, non-destructive `nps.project/v1` to `v2` migration that preserves
  history, transactions, idempotency, source objects, and recovery semantics.
- Verified atomic 16-bit P6 reference export tied to authoritative v2 snapshot
  identity, with process-level crash and retry coverage.

### Fixed

- Replaced the renderer/export dependency on incomplete standard-library
  `std::stop_token` implementations with a project-owned shared atomic
  cancellation primitive, preserving C++20 portability on Apple Silicon.
- Completed aggregate initialization for all persisted command fields so GCC
  warning-as-error builds reject no valid M1 source or test target.

### Security

- Export destinations are checked against mandatory project safety scopes,
  filesystem aliases, Windows reparse/ADS/device-name hazards, and POSIX
  canonical parents before an exclusive same-directory temporary is created.
- Windows replacement avoids `ReplaceFileW` data-loss and alternate-stream
  inheritance behavior; macOS uses `F_FULLFSYNC` when available.
- Export safety scopes must resolve to existing filesystem objects, closing
  missing-leaf case-alias gaps; Windows UNC and mapped remote volumes are
  rejected before temporary-file creation.
- Canonical source and mask hashes are recomputed before rendering, and cache
  admission occurs only after the complete render succeeds.
- Project open and migration require the exact versioned SQLite schema and
  metadata key set before any state write or transform; triggers, views, custom
  indexes, extra objects, and weakened DDL are rejected with privacy-safe errors.
- Migration revalidates the backed-up v1 schema and source fingerprint, then
  verifies the candidate v2's preserved v1 projection before publication.
- Render tile grids are generated lazily and capped at 65,536 entries across
  identity, cold-cache, and hot-cache paths to bound adversarial scheduler state.
- Exposure edits chain from the graph's declared output node, and unsupported
  command properties are rejected without echoing attacker-controlled names.

## [0.1.0] - 2026-07-25

- Initial product, UX, architecture, privacy, quality, model governance, operations, and roadmap design set.
