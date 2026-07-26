#pragma once

#include "nps/document/edit_graph.hpp"
#include "nps/imaging/image_f32.hpp"
#include "nps/imaging/mask16.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>

namespace nps::render {

struct ImageRect final {
  std::uint32_t x{};
  std::uint32_t y{};
  std::uint32_t width{};
  std::uint32_t height{};

  [[nodiscard]] bool operator==(const ImageRect&) const = default;
};

enum class RenderQuality {
  draft,
  interactive,
  final,
};

// Bounds scheduling overhead even when diagnostics request one-pixel tiles.
// The clipped ROI must compile to no more than this many tiles.
inline constexpr std::size_t kMaximumTilesPerRenderRequest = 65'536U;

struct RenderRequest final {
  std::string document_id;
  std::string snapshot_id;
  std::int64_t revision{};
  // Canonical image_f32_sha256() of the decoded scene-linear source buffer.
  // This is intentionally distinct from a project object's encoded-file hash.
  std::string source_hash;
  // Monotonically increasing per-document request sequence. It makes two
  // otherwise identical in-flight requests distinguishable at publication.
  std::uint64_t generation{};
  ImageRect roi;
  RenderQuality quality{RenderQuality::final};
  // 512 is the M1 production reference size. Smaller values are accepted so
  // tests and diagnostics can force boundary crossings. Values above 512, or
  // an ROI/tile combination exceeding kMaximumTilesPerRenderRequest, are
  // rejected before scheduling or output allocation.
  std::uint32_t tile_size{512};
  std::uint32_t worker_count{1};
  std::size_t maximum_output_bytes{
      static_cast<std::size_t>(1024) * 1024U * 1024U};
  std::size_t maximum_transient_bytes{
      static_cast<std::size_t>(512) * 1024U * 1024U};
};

struct MaskAssetView final {
  // Owned strings avoid dangling views when identities are produced by
  // canonical hash helpers or short-lived scheduling objects.
  std::string mask_id;
  std::string content_hash;
  const imaging::Mask16* mask{};
};

struct RenderDiagnostics final {
  std::size_t rendered_tiles{};
  std::size_t cache_hits{};
  std::size_t cache_misses{};
  std::size_t peak_transient_tile_bytes{};
  std::size_t peak_transient_working_set_bytes{};
  std::uint32_t worker_count{};

  [[nodiscard]] bool operator==(const RenderDiagnostics&) const = default;
};

struct RenderIdentity final {
  std::string document_id;
  std::string snapshot_id;
  std::int64_t revision{};
  std::string source_hash;
  std::string graph_hash;
  // Lowercase SHA-256 over exactly the mask IDs and canonical content hashes
  // referenced by the graph.
  std::string mask_signature;
  ImageRect roi;
  RenderQuality quality{RenderQuality::final};
  std::uint64_t generation{};

  [[nodiscard]] bool operator==(const RenderIdentity&) const = default;
};

struct RenderResult final {
  RenderIdentity identity;
  imaging::ImageF32 image;
  RenderDiagnostics diagnostics;
};

enum class RenderErrorCode {
  invalid_request,
  unsupported_graph,
  missing_mask,
  mask_dimension_mismatch,
  empty_region,
  cancelled,
  numeric_failure,
};

[[nodiscard]] std::string_view to_string(RenderErrorCode code) noexcept;

class RenderError final : public std::runtime_error {
 public:
  RenderError(RenderErrorCode code, std::string message);

  [[nodiscard]] RenderErrorCode code() const noexcept;

 private:
  RenderErrorCode code_;
};

struct TileCacheStats final {
  std::size_t entry_count{};
  // Includes sample storage, the fixed-size key, and conservative
  // list/index bookkeeping rather than counting pixel payload alone.
  std::size_t resident_bytes{};
  std::size_t capacity_bytes{};

  [[nodiscard]] bool operator==(const TileCacheStats&) const = default;
};

class CpuTileCache final {
 public:
  explicit CpuTileCache(std::size_t capacity_bytes);
  ~CpuTileCache();

  CpuTileCache(CpuTileCache&&) noexcept;
  CpuTileCache& operator=(CpuTileCache&&) noexcept;
  CpuTileCache(const CpuTileCache&) = delete;
  CpuTileCache& operator=(const CpuTileCache&) = delete;

  [[nodiscard]] TileCacheStats stats() const;
  void clear() noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;

  friend class CpuRenderer;
};

class CpuRenderer final {
 public:
  CpuRenderer() = default;
  explicit CpuRenderer(std::shared_ptr<CpuTileCache> cache) noexcept;

  // M1 uses the same exact CPU algorithm for all quality labels. The label is
  // still part of the request/result contract so later preview approximations
  // cannot be confused with final output.
  // source and every non-null MaskAssetView::mask must outlive this synchronous
  // call and must not be mutated concurrently. The renderer owns its result
  // and never retains those input pointers after return.
  [[nodiscard]] RenderResult render(
      const imaging::ImageF32& source,
      const document::EditGraph& graph,
      std::span<const MaskAssetView> masks,
      const RenderRequest& request,
      std::stop_token stop_token = {}) const;

 private:
  std::shared_ptr<CpuTileCache> cache_;
};

// Validates the source, graph, mask bindings, and request, then returns the
// exact identity that render() will attach to its result. Callers can install
// this identity in a RenderPublicationGate before dispatching background work.
[[nodiscard]] RenderIdentity make_render_identity(
    const imaging::ImageF32& source,
    const document::EditGraph& graph,
    std::span<const MaskAssetView> masks,
    const RenderRequest& request);

[[nodiscard]] bool is_render_result_current(
    const RenderResult& result,
    const RenderIdentity& expected) noexcept;

// Thread-safe linearization point for background preview/final render results.
// Updating the expected identity invalidates the previously published result.
class RenderPublicationGate final {
 public:
  explicit RenderPublicationGate(RenderIdentity expected);
  ~RenderPublicationGate();

  RenderPublicationGate(RenderPublicationGate&&) noexcept;
  RenderPublicationGate& operator=(RenderPublicationGate&&) noexcept;
  RenderPublicationGate(const RenderPublicationGate&) = delete;
  RenderPublicationGate& operator=(const RenderPublicationGate&) = delete;

  void expect(RenderIdentity expected);
  [[nodiscard]] bool try_publish(RenderResult result);
  [[nodiscard]] RenderIdentity expected() const;
  [[nodiscard]] std::shared_ptr<const RenderResult> current() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace nps::render
