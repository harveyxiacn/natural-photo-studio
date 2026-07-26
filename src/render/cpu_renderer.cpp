#include "nps/render/cpu_renderer.hpp"

#include "nps/color/color_encoding.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <exception>
#include <list>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include <openssl/evp.h>

namespace nps::render {
namespace {

constexpr std::uint32_t kMaximumTileSize = 512;
constexpr std::uint32_t kMaximumWorkers = 64;
constexpr std::size_t kSha256Bytes = 32U;
constexpr std::size_t kCacheEntryAccountingOverhead = 128U;

[[noreturn]] void throw_render_error(
    const RenderErrorCode code,
    std::string message) {
  throw RenderError(code, std::move(message));
}

[[nodiscard]] bool is_lower_sha256(const std::string_view value) noexcept {
  if (value.size() != 64U) {
    return false;
  }
  return std::ranges::all_of(value, [](const char character) {
    return (character >= '0' && character <= '9') ||
           (character >= 'a' && character <= 'f');
  });
}

[[nodiscard]] bool is_stable_identifier(
    const std::string_view value) noexcept {
  if (value.empty() || value.size() > 128U) {
    return false;
  }
  const auto is_alphanumeric = [](const char character) {
    return (character >= '0' && character <= '9') ||
           (character >= 'a' && character <= 'z') ||
           (character >= 'A' && character <= 'Z');
  };
  if (!is_alphanumeric(value.front())) {
    return false;
  }
  return std::ranges::all_of(value, [&](const char character) {
    return is_alphanumeric(character) || character == '.' ||
           character == '_' || character == ':' || character == '-';
  });
}

[[nodiscard]] std::string sha256_hex(const std::string_view value) {
  std::array<unsigned char, kSha256Bytes> digest{};
  unsigned int digest_size = 0U;
  if (EVP_Digest(
          value.data(),
          value.size(),
          digest.data(),
          &digest_size,
          EVP_sha256(),
          nullptr) != 1 ||
      digest_size != digest.size()) {
    throw std::runtime_error{"SHA-256 digest calculation failed"};
  }

  constexpr std::string_view hexadecimal = "0123456789abcdef";
  std::string result;
  result.resize(kSha256Bytes * 2U);
  for (std::size_t index = 0U; index < digest.size(); ++index) {
    result[index * 2U] = hexadecimal[digest[index] >> 4U];
    result[index * 2U + 1U] =
        hexadecimal[digest[index] & 0x0FU];
  }
  return result;
}

void validate_render_identity(const RenderIdentity& identity) {
  if (!is_stable_identifier(identity.document_id) ||
      !is_stable_identifier(identity.snapshot_id) ||
      identity.revision < 0 ||
      !is_lower_sha256(identity.source_hash) ||
      !is_lower_sha256(identity.graph_hash) ||
      !is_lower_sha256(identity.mask_signature) ||
      identity.roi.width == 0U || identity.roi.height == 0U) {
    throw std::invalid_argument{
        "A render identity contains an invalid identifier, hash, revision, "
        "or region."};
  }
  switch (identity.quality) {
    case RenderQuality::draft:
    case RenderQuality::interactive:
    case RenderQuality::final:
      return;
  }
  throw std::invalid_argument{
      "A render identity contains an invalid quality label."};
}

[[nodiscard]] std::size_t checked_multiply(
    const std::size_t left,
    const std::size_t right) {
  if (right != 0U &&
      left > std::numeric_limits<std::size_t>::max() / right) {
    throw_render_error(
        RenderErrorCode::invalid_request,
        "The render dimensions exceed the addressable memory range.");
  }
  return left * right;
}

[[nodiscard]] ImageRect validate_request(
    const imaging::ImageF32& source,
    const RenderRequest& request) {
  if (!is_stable_identifier(request.document_id) ||
      !is_stable_identifier(request.snapshot_id)) {
    throw_render_error(
        RenderErrorCode::invalid_request,
        "The render document and snapshot IDs must be stable identifiers.");
  }
  if (request.revision < 0) {
    throw_render_error(
        RenderErrorCode::invalid_request,
        "The render revision cannot be negative.");
  }
  if (!is_lower_sha256(request.source_hash)) {
    throw_render_error(
        RenderErrorCode::invalid_request,
        "The render source hash must be a lowercase SHA-256 value.");
  }
  switch (request.quality) {
    case RenderQuality::draft:
    case RenderQuality::interactive:
    case RenderQuality::final:
      break;
    default:
      throw_render_error(
          RenderErrorCode::invalid_request,
          "The render quality is not supported.");
  }
  if (request.roi.width == 0U || request.roi.height == 0U) {
    throw_render_error(
        RenderErrorCode::invalid_request,
        "The render region must be non-empty.");
  }
  const std::uint64_t right =
      static_cast<std::uint64_t>(request.roi.x) + request.roi.width;
  const std::uint64_t bottom =
      static_cast<std::uint64_t>(request.roi.y) + request.roi.height;
  const std::uint32_t clipped_x =
      std::min(request.roi.x, source.width);
  const std::uint32_t clipped_y =
      std::min(request.roi.y, source.height);
  const std::uint64_t clipped_right =
      std::min<std::uint64_t>(right, source.width);
  const std::uint64_t clipped_bottom =
      std::min<std::uint64_t>(bottom, source.height);
  if (clipped_right <= clipped_x || clipped_bottom <= clipped_y) {
    throw_render_error(
        RenderErrorCode::empty_region,
        "The render region does not intersect the source image.");
  }
  const ImageRect effective_roi{
      .x = clipped_x,
      .y = clipped_y,
      .width = static_cast<std::uint32_t>(clipped_right - clipped_x),
      .height = static_cast<std::uint32_t>(clipped_bottom - clipped_y)};
  if (request.tile_size == 0U ||
      request.tile_size > kMaximumTileSize) {
    throw_render_error(
        RenderErrorCode::invalid_request,
        "The tile size is outside the supported range.");
  }
  if (request.worker_count == 0U ||
      request.worker_count > kMaximumWorkers) {
    throw_render_error(
        RenderErrorCode::invalid_request,
        "The worker count is outside the supported range.");
  }

  const std::size_t pixels = checked_multiply(
      static_cast<std::size_t>(effective_roi.width),
      static_cast<std::size_t>(effective_roi.height));
  const std::size_t bytes = checked_multiply(
      checked_multiply(
          pixels,
          imaging::ImageF32::channel_count),
      sizeof(float));
  if (bytes > request.maximum_output_bytes) {
    throw_render_error(
        RenderErrorCode::invalid_request,
        "The requested output exceeds the configured memory budget.");
  }
  return effective_roi;
}

struct SuppliedMask final {
  const imaging::Mask16* mask{};
  std::string_view content_hash;
};

[[nodiscard]] const SuppliedMask* find_mask(
    const std::unordered_map<std::string_view, SuppliedMask>& masks,
    const std::string_view mask_id) {
  const auto found = masks.find(mask_id);
  return found == masks.end() ? nullptr : &found->second;
}

struct ExecutableNode final {
  const document::EditNode* node{};
  const imaging::Mask16* mask{};
};

[[nodiscard]] std::vector<ExecutableNode> compile_linear_chain(
    const imaging::ImageF32& source,
    const document::EditGraph& graph,
    const std::span<const MaskAssetView> mask_views) {
  const auto graph_encoding =
      color::color_encoding_from_id(graph.working_color_space());
  if (!graph_encoding.has_value() ||
      *graph_encoding != source.encoding) {
    throw_render_error(
        RenderErrorCode::unsupported_graph,
        "The graph working color space does not match the source image.");
  }

  std::unordered_map<std::string_view, SuppliedMask> masks;
  if (mask_views.size() > document::kMaximumEditGraphNodes) {
    throw_render_error(
        RenderErrorCode::invalid_request,
        "The render mask collection exceeds the supported limit.");
  }
  masks.reserve(mask_views.size());
  for (const MaskAssetView& view : mask_views) {
    if (!is_stable_identifier(view.mask_id)) {
      throw_render_error(
          RenderErrorCode::invalid_request,
          "The mask view collection contains an invalid stable ID.");
    }
    if (!is_lower_sha256(view.content_hash)) {
      throw_render_error(
          RenderErrorCode::invalid_request,
          "The mask view collection contains an invalid content hash.");
    }
    if (view.mask == nullptr) {
      throw_render_error(
          RenderErrorCode::invalid_request,
          "The mask view collection contains a null asset pointer.");
    }
    if (!masks
             .emplace(
                 view.mask_id,
                 SuppliedMask{
                     .mask = view.mask,
                     .content_hash = view.content_hash})
             .second) {
      throw_render_error(
          RenderErrorCode::invalid_request,
          "The mask view collection contains a duplicate ID.");
    }
    const std::string actual_content_hash =
        imaging::mask16_sha256(*view.mask);
    if (actual_content_hash != view.content_hash) {
      throw_render_error(
          RenderErrorCode::invalid_request,
          "A supplied mask content hash does not match its canonical "
          "samples.");
    }
    if (view.mask->width != source.width ||
        view.mask->height != source.height) {
      throw_render_error(
          RenderErrorCode::mask_dimension_mismatch,
          "A render mask does not match the source dimensions.");
    }
  }

  const document::EditNode* current =
      graph.find_node(graph.output_node_id());
  if (current == nullptr ||
      current->kind != document::EditNodeKind::output ||
      current->inputs.size() != 1U) {
    throw_render_error(
        RenderErrorCode::unsupported_graph,
        "The M1 CPU renderer requires a single-input output node.");
  }

  std::vector<const document::EditNode*> reverse_chain;
  std::unordered_set<std::string_view> visited;
  while (current->kind != document::EditNodeKind::source) {
    if (!visited.emplace(current->node_id).second ||
        current->inputs.size() != 1U) {
      throw_render_error(
          RenderErrorCode::unsupported_graph,
          "The M1 CPU renderer requires one acyclic adjustment chain.");
    }
    reverse_chain.push_back(current);
    current = graph.find_node(current->inputs.front());
    if (current == nullptr) {
      throw_render_error(
          RenderErrorCode::unsupported_graph,
          "The render graph contains a missing input.");
    }
  }
  if (current->node_id != graph.source_node_id() ||
      !current->inputs.empty()) {
    throw_render_error(
        RenderErrorCode::unsupported_graph,
        "The render chain does not terminate at the declared source.");
  }
  visited.emplace(current->node_id);
  if (visited.size() != graph.nodes().size()) {
    throw_render_error(
        RenderErrorCode::unsupported_graph,
        "The M1 CPU renderer does not accept unreachable or branched nodes.");
  }

  std::ranges::reverse(reverse_chain);
  std::vector<ExecutableNode> executable;
  executable.reserve(reverse_chain.size());
  for (const document::EditNode* node : reverse_chain) {
    if (node->kind == document::EditNodeKind::output) {
      continue;
    }
    if (node->kind != document::EditNodeKind::adjust_exposure &&
        node->kind != document::EditNodeKind::adjust_curve_rgb) {
      throw_render_error(
          RenderErrorCode::unsupported_graph,
          "The graph contains a node unsupported by the M1 CPU renderer.");
    }
    const imaging::Mask16* mask = nullptr;
    if (node->mask.has_value()) {
      const SuppliedMask* supplied =
          find_mask(masks, node->mask->mask_id);
      if (supplied == nullptr) {
        throw_render_error(
            RenderErrorCode::missing_mask,
            "The graph references a mask that was not supplied.");
      }
      if (supplied->content_hash != node->mask->content_hash) {
        throw_render_error(
            RenderErrorCode::invalid_request,
            "The supplied mask identity does not match the edit graph.");
      }
      mask = supplied->mask;
    }
    executable.push_back(ExecutableNode{.node = node, .mask = mask});
  }
  return executable;
}

[[nodiscard]] double evaluate_curve(
    const std::vector<document::CurveControlPoint>& points,
    const double value) {
  const auto upper = std::ranges::upper_bound(
      points,
      value,
      {},
      &document::CurveControlPoint::x);
  const document::CurveControlPoint* left = nullptr;
  const document::CurveControlPoint* right = nullptr;
  if (upper == points.begin()) {
    left = &points[0];
    right = &points[1];
  } else if (upper == points.end()) {
    left = &points[points.size() - 2U];
    right = &points.back();
  } else {
    right = &*upper;
    left = &*(upper - 1);
  }

  const double scale = (value - left->x) / (right->x - left->x);
  return left->y + scale * (right->y - left->y);
}

[[nodiscard]] double mask_coverage(
    const ExecutableNode& executable,
    const std::uint32_t source_x,
    const std::uint32_t source_y) {
  double coverage = executable.node->opacity;
  if (executable.mask != nullptr) {
    const std::size_t mask_index =
        static_cast<std::size_t>(source_y) * executable.mask->width +
        source_x;
    double mask_value =
        static_cast<double>(executable.mask->samples[mask_index]) /
        static_cast<double>(imaging::Mask16::maximum);
    if (executable.node->mask->inverted) {
      mask_value = 1.0 - mask_value;
    }
    coverage *= mask_value;
  }
  return coverage;
}

void apply_node(
    std::vector<float>& tile_samples,
    const std::uint32_t tile_width,
    const std::uint32_t tile_height,
    const std::uint32_t source_x,
    const std::uint32_t source_y,
    const ExecutableNode& executable,
    const std::stop_token stop_token) {
  const document::EditNode& node = *executable.node;
  if (!node.enabled || node.opacity == 0.0) {
    return;
  }

  double exposure_multiplier = 1.0;
  const std::vector<document::CurveControlPoint>* curve = nullptr;
  if (node.kind == document::EditNodeKind::adjust_exposure) {
    exposure_multiplier = std::exp2(
        std::get<document::ExposureParameters>(node.parameters).ev);
  } else {
    curve = &std::get<document::RgbCurveParameters>(
                 node.parameters)
                 .points;
  }

  for (std::uint32_t y = 0; y < tile_height; ++y) {
    if (stop_token.stop_requested()) {
      throw_render_error(
          RenderErrorCode::cancelled,
          "The render was cancelled before completion.");
    }
    for (std::uint32_t x = 0; x < tile_width; ++x) {
      const std::size_t pixel_index =
          (static_cast<std::size_t>(y) * tile_width + x) *
          imaging::ImageF32::channel_count;
      const double alpha = tile_samples[pixel_index + 3U];
      const double coverage = mask_coverage(
          executable, source_x + x, source_y + y);
      for (std::size_t channel = 0; channel < 3U; ++channel) {
        const double original = tile_samples[pixel_index + channel];
        double adjusted = original;
        if (node.kind == document::EditNodeKind::adjust_exposure) {
          adjusted = original * exposure_multiplier;
        } else if (alpha != 0.0) {
          const double straight = original / alpha;
          adjusted = evaluate_curve(*curve, straight) * alpha;
        }
        const double blended =
            original + (adjusted - original) * coverage;
        if (!std::isfinite(blended) ||
            blended < -static_cast<double>(
                          std::numeric_limits<float>::max()) ||
            blended > static_cast<double>(
                          std::numeric_limits<float>::max())) {
          throw_render_error(
              RenderErrorCode::numeric_failure,
              "A render node produced a non-finite pixel value.");
        }
        tile_samples[pixel_index + channel] =
            static_cast<float>(blended);
      }
    }
  }
}

struct Tile final {
  std::uint32_t x{};
  std::uint32_t y{};
  std::uint32_t width{};
  std::uint32_t height{};
};

struct TileGrid final {
  ImageRect roi;
  std::uint32_t tile_size{};
  std::size_t columns{};
  std::size_t tile_count{};
};

[[nodiscard]] TileGrid make_tile_grid(
    const ImageRect roi,
    const std::uint32_t tile_size) {
  const std::uint64_t columns =
      (static_cast<std::uint64_t>(roi.width) + tile_size - 1U) /
      tile_size;
  const std::uint64_t rows =
      (static_cast<std::uint64_t>(roi.height) + tile_size - 1U) /
      tile_size;
  if (columns > std::numeric_limits<std::size_t>::max() ||
      rows > std::numeric_limits<std::size_t>::max()) {
    throw_render_error(
        RenderErrorCode::invalid_request,
        "The render tile grid exceeds the addressable range.");
  }
  const std::size_t tile_count = checked_multiply(
      static_cast<std::size_t>(columns),
      static_cast<std::size_t>(rows));
  if (tile_count == 0U ||
      tile_count > kMaximumTilesPerRenderRequest) {
    throw_render_error(
        RenderErrorCode::invalid_request,
        "The render tile grid exceeds the supported request limit.");
  }
  return TileGrid{
      .roi = roi,
      .tile_size = tile_size,
      .columns = static_cast<std::size_t>(columns),
      .tile_count = tile_count};
}

[[nodiscard]] Tile tile_at(
    const TileGrid& grid,
    const std::size_t index) {
  if (index >= grid.tile_count) {
    throw std::logic_error{"A render worker requested an invalid tile index."};
  }
  const std::size_t column = index % grid.columns;
  const std::size_t row = index / grid.columns;
  const std::uint64_t x =
      static_cast<std::uint64_t>(grid.roi.x) +
      static_cast<std::uint64_t>(column) * grid.tile_size;
  const std::uint64_t y =
      static_cast<std::uint64_t>(grid.roi.y) +
      static_cast<std::uint64_t>(row) * grid.tile_size;
  const std::uint64_t right =
      static_cast<std::uint64_t>(grid.roi.x) + grid.roi.width;
  const std::uint64_t bottom =
      static_cast<std::uint64_t>(grid.roi.y) + grid.roi.height;
  return Tile{
      .x = static_cast<std::uint32_t>(x),
      .y = static_cast<std::uint32_t>(y),
      .width = static_cast<std::uint32_t>(
          std::min<std::uint64_t>(grid.tile_size, right - x)),
      .height = static_cast<std::uint32_t>(
          std::min<std::uint64_t>(grid.tile_size, bottom - y))};
}

[[nodiscard]] std::vector<float> render_tile_samples(
    const imaging::ImageF32& source,
    const Tile tile,
    const std::vector<ExecutableNode>& executable,
    const std::stop_token stop_token) {
  const std::size_t sample_count = checked_multiply(
      checked_multiply(
          static_cast<std::size_t>(tile.width),
          static_cast<std::size_t>(tile.height)),
      imaging::ImageF32::channel_count);
  std::vector<float> tile_samples(sample_count);

  for (std::uint32_t y = 0; y < tile.height; ++y) {
    const std::size_t source_offset =
        (static_cast<std::size_t>(tile.y + y) * source.width + tile.x) *
        imaging::ImageF32::channel_count;
    const std::size_t tile_offset =
        static_cast<std::size_t>(y) * tile.width *
        imaging::ImageF32::channel_count;
    std::ranges::copy_n(
        source.samples.begin() +
            static_cast<std::ptrdiff_t>(source_offset),
        static_cast<std::ptrdiff_t>(
            static_cast<std::size_t>(tile.width) *
            imaging::ImageF32::channel_count),
        tile_samples.begin() +
            static_cast<std::ptrdiff_t>(tile_offset));
  }

  for (const ExecutableNode& node : executable) {
    apply_node(
        tile_samples,
        tile.width,
        tile.height,
        tile.x,
        tile.y,
        node,
        stop_token);
  }

  return tile_samples;
}

void copy_tile_to_output(
    const std::vector<float>& tile_samples,
    imaging::ImageF32& output,
    const ImageRect output_roi,
    const Tile tile) {
  for (std::uint32_t y = 0; y < tile.height; ++y) {
    const std::size_t tile_offset =
        static_cast<std::size_t>(y) * tile.width *
        imaging::ImageF32::channel_count;
    const std::uint32_t output_x = tile.x - output_roi.x;
    const std::uint32_t output_y = tile.y - output_roi.y + y;
    const std::size_t output_offset =
        (static_cast<std::size_t>(output_y) * output.width + output_x) *
        imaging::ImageF32::channel_count;
    std::ranges::copy_n(
        tile_samples.begin() +
            static_cast<std::ptrdiff_t>(tile_offset),
        static_cast<std::ptrdiff_t>(
            static_cast<std::size_t>(tile.width) *
            imaging::ImageF32::channel_count),
        output.samples.begin() +
            static_cast<std::ptrdiff_t>(output_offset));
  }
}

[[nodiscard]] std::vector<float> copy_tile_from_output(
    const imaging::ImageF32& output,
    const ImageRect output_roi,
    const Tile tile) {
  const std::size_t sample_count = checked_multiply(
      checked_multiply(
          static_cast<std::size_t>(tile.width),
          static_cast<std::size_t>(tile.height)),
      imaging::ImageF32::channel_count);
  std::vector<float> tile_samples(sample_count);
  for (std::uint32_t y = 0; y < tile.height; ++y) {
    const std::uint32_t output_x = tile.x - output_roi.x;
    const std::uint32_t output_y = tile.y - output_roi.y + y;
    const std::size_t output_offset =
        (static_cast<std::size_t>(output_y) * output.width + output_x) *
        imaging::ImageF32::channel_count;
    const std::size_t tile_offset =
        static_cast<std::size_t>(y) * tile.width *
        imaging::ImageF32::channel_count;
    std::ranges::copy_n(
        output.samples.begin() +
            static_cast<std::ptrdiff_t>(output_offset),
        static_cast<std::ptrdiff_t>(
            static_cast<std::size_t>(tile.width) *
            imaging::ImageF32::channel_count),
        tile_samples.begin() +
            static_cast<std::ptrdiff_t>(tile_offset));
  }
  return tile_samples;
}

void append_cache_key_field(
    std::string& key,
    const std::string_view value) {
  key += std::to_string(value.size());
  key.push_back(':');
  key.append(value);
  key.push_back('|');
}

[[nodiscard]] std::string make_mask_signature(
    const std::vector<ExecutableNode>& executable) {
  std::vector<std::pair<std::string_view, std::string_view>> identities;
  identities.reserve(executable.size());
  for (const ExecutableNode& node : executable) {
    if (node.node->mask.has_value()) {
      identities.emplace_back(
          node.node->mask->mask_id,
          node.node->mask->content_hash);
    }
  }
  std::ranges::sort(identities);
  const auto unique_end = std::ranges::unique(identities).begin();
  identities.erase(unique_end, identities.end());

  std::string signature{"nps.render.mask-signature/v1"};
  for (const auto& [mask_id, content_hash] : identities) {
    append_cache_key_field(signature, mask_id);
    append_cache_key_field(signature, content_hash);
  }
  return sha256_hex(signature);
}

[[nodiscard]] std::string make_cache_key(
    const RenderIdentity& identity,
    const std::string_view color_encoding,
    const Tile tile) {
  std::string material{"nps.render.tile-cache-key/v1"};
  material.reserve(768U);
  append_cache_key_field(material, identity.document_id);
  append_cache_key_field(material, identity.snapshot_id);
  append_cache_key_field(material, identity.source_hash);
  append_cache_key_field(material, identity.graph_hash);
  append_cache_key_field(material, color_encoding);
  append_cache_key_field(material, identity.mask_signature);
  append_cache_key_field(material, std::to_string(identity.revision));
  append_cache_key_field(
      material,
      std::to_string(static_cast<std::uint32_t>(identity.quality)));
  append_cache_key_field(material, std::to_string(identity.generation));
  append_cache_key_field(material, std::to_string(identity.roi.x));
  append_cache_key_field(material, std::to_string(identity.roi.y));
  append_cache_key_field(material, std::to_string(identity.roi.width));
  append_cache_key_field(material, std::to_string(identity.roi.height));
  append_cache_key_field(material, std::to_string(tile.x));
  append_cache_key_field(material, std::to_string(tile.y));
  append_cache_key_field(material, std::to_string(tile.width));
  append_cache_key_field(material, std::to_string(tile.height));
  return sha256_hex(material);
}

struct PreparedRender final {
  ImageRect effective_roi;
  TileGrid tile_grid;
  std::vector<ExecutableNode> executable;
  RenderIdentity identity;
  std::string_view color_encoding;
};

[[nodiscard]] PreparedRender prepare_render(
    const imaging::ImageF32& source,
    const document::EditGraph& graph,
    const std::span<const MaskAssetView> masks,
    const RenderRequest& request) {
  try {
    const std::string actual_source_hash =
        imaging::image_f32_sha256(source);
    const ImageRect effective_roi = validate_request(source, request);
    const TileGrid tile_grid =
        make_tile_grid(effective_roi, request.tile_size);
    if (actual_source_hash != request.source_hash) {
      throw_render_error(
          RenderErrorCode::invalid_request,
          "The claimed render source hash does not match the canonical "
          "image samples.");
    }

    std::vector<ExecutableNode> executable =
        compile_linear_chain(source, graph, masks);
    RenderIdentity identity{
        .document_id = request.document_id,
        .snapshot_id = request.snapshot_id,
        .revision = request.revision,
        .source_hash = actual_source_hash,
        .graph_hash = document::edit_graph_sha256(graph),
        .mask_signature = make_mask_signature(executable),
        .roi = effective_roi,
        .quality = request.quality,
        .generation = request.generation};
    validate_render_identity(identity);
    return PreparedRender{
        .effective_roi = effective_roi,
        .tile_grid = tile_grid,
        .executable = std::move(executable),
        .identity = std::move(identity),
        .color_encoding = color::color_encoding_id(source.encoding)};
  } catch (const RenderError&) {
    throw;
  } catch (const std::invalid_argument&) {
    throw_render_error(
        RenderErrorCode::invalid_request,
        "The render source, graph, or mask asset is invalid.");
  }
}

}  // namespace

struct CpuTileCache::Impl final {
  struct Entry final {
    std::string key;
    std::vector<float> samples;
    std::size_t accounted_bytes{};
  };

  explicit Impl(const std::size_t capacity) : capacity_bytes(capacity) {}

  [[nodiscard]] std::optional<std::vector<float>> lookup(
      const std::string_view key) {
    std::scoped_lock lock(mutex);
    const auto found = entries.find(key);
    if (found == entries.end()) {
      return std::nullopt;
    }
    recency.splice(recency.begin(), recency, found->second);
    return found->second->samples;
  }

  void store(std::string key, std::vector<float> samples) {
    if (!is_lower_sha256(key)) {
      return;
    }
    constexpr std::size_t fixed_accounting =
        sizeof(Entry) + sizeof(std::string_view) +
        sizeof(std::list<Entry>::iterator) +
        kCacheEntryAccountingOverhead;
    if (samples.capacity() >
        std::numeric_limits<std::size_t>::max() / sizeof(float)) {
      return;
    }
    const std::size_t sample_bytes = samples.capacity() * sizeof(float);
    if (sample_bytes >
        std::numeric_limits<std::size_t>::max() - fixed_accounting ||
        sample_bytes + fixed_accounting >
            std::numeric_limits<std::size_t>::max() - key.size()) {
      return;
    }
    const std::size_t accounted_bytes =
        sample_bytes + fixed_accounting + key.size();
    if (accounted_bytes > capacity_bytes) {
      return;
    }

    std::scoped_lock lock(mutex);
    if (const auto existing = entries.find(std::string_view{key});
        existing != entries.end()) {
      recency.splice(recency.begin(), recency, existing->second);
      return;
    }

    recency.push_front(Entry{
        .key = std::move(key),
        .samples = std::move(samples),
        .accounted_bytes = accounted_bytes});
    const auto position = recency.begin();
    try {
      const auto [ignored, inserted] = entries.emplace(
          std::string_view{position->key}, position);
      static_cast<void>(ignored);
      if (!inserted) {
        recency.pop_front();
        return;
      }
    } catch (...) {
      recency.pop_front();
      throw;
    }
    resident_bytes += accounted_bytes;

    while (resident_bytes > capacity_bytes && !recency.empty()) {
      const Entry& victim = recency.back();
      resident_bytes -= victim.accounted_bytes;
      entries.erase(std::string_view{victim.key});
      recency.pop_back();
    }
  }

  [[nodiscard]] TileCacheStats stats() const {
    std::scoped_lock lock(mutex);
    return TileCacheStats{
        .entry_count = entries.size(),
        .resident_bytes = resident_bytes,
        .capacity_bytes = capacity_bytes};
  }

  void clear() noexcept {
    std::scoped_lock lock(mutex);
    entries.clear();
    recency.clear();
    resident_bytes = 0U;
  }

  std::size_t capacity_bytes{};
  std::size_t resident_bytes{};
  mutable std::mutex mutex;
  std::list<Entry> recency;
  std::unordered_map<
      std::string_view,
      std::list<Entry>::iterator>
      entries;
};

CpuTileCache::CpuTileCache(const std::size_t capacity_bytes)
    : impl_(std::make_unique<Impl>(capacity_bytes)) {
  if (capacity_bytes == 0U) {
    throw std::invalid_argument(
        "A CPU tile cache must have a non-zero byte capacity.");
  }
}

CpuTileCache::~CpuTileCache() = default;
CpuTileCache::CpuTileCache(CpuTileCache&&) noexcept = default;
CpuTileCache& CpuTileCache::operator=(CpuTileCache&&) noexcept = default;

TileCacheStats CpuTileCache::stats() const {
  if (impl_ == nullptr) {
    return {};
  }
  return impl_->stats();
}

void CpuTileCache::clear() noexcept {
  if (impl_ != nullptr) {
    impl_->clear();
  }
}

std::string_view to_string(const RenderErrorCode code) noexcept {
  switch (code) {
    case RenderErrorCode::invalid_request:
      return "RENDER_INVALID_REQUEST";
    case RenderErrorCode::unsupported_graph:
      return "RENDER_UNSUPPORTED_GRAPH";
    case RenderErrorCode::missing_mask:
      return "RENDER_MISSING_MASK";
    case RenderErrorCode::mask_dimension_mismatch:
      return "RENDER_MASK_DIMENSION_MISMATCH";
    case RenderErrorCode::empty_region:
      return "RENDER_EMPTY_REGION";
    case RenderErrorCode::cancelled:
      return "RENDER_CANCELLED";
    case RenderErrorCode::numeric_failure:
      return "RENDER_NUMERIC_FAILURE";
  }
  return "RENDER_UNKNOWN_ERROR";
}

RenderError::RenderError(
    const RenderErrorCode code,
    std::string message)
    : std::runtime_error(std::move(message)), code_(code) {}

RenderErrorCode RenderError::code() const noexcept { return code_; }

CpuRenderer::CpuRenderer(std::shared_ptr<CpuTileCache> cache) noexcept
    : cache_(std::move(cache)) {}

RenderIdentity make_render_identity(
    const imaging::ImageF32& source,
    const document::EditGraph& graph,
    const std::span<const MaskAssetView> masks,
    const RenderRequest& request) {
  return prepare_render(source, graph, masks, request).identity;
}

RenderResult CpuRenderer::render(
    const imaging::ImageF32& source,
    const document::EditGraph& graph,
    const std::span<const MaskAssetView> masks,
    const RenderRequest& request,
    const std::stop_token stop_token) const {
  if (stop_token.stop_requested()) {
    throw_render_error(
        RenderErrorCode::cancelled,
        "The render was cancelled before it started.");
  }

  PreparedRender prepared =
      prepare_render(source, graph, masks, request);
  const ImageRect effective_roi = prepared.effective_roi;
  const TileGrid& tile_grid = prepared.tile_grid;
  const std::uint32_t actual_workers = static_cast<std::uint32_t>(
      std::min<std::size_t>(request.worker_count, tile_grid.tile_count));
  const std::size_t peak_tile_pixels = checked_multiply(
      static_cast<std::size_t>(
          std::min(request.tile_size, effective_roi.width)),
      static_cast<std::size_t>(
          std::min(request.tile_size, effective_roi.height)));
  const std::size_t peak_tile_bytes = checked_multiply(
      checked_multiply(
          peak_tile_pixels,
          imaging::ImageF32::channel_count),
      sizeof(float));
  const std::size_t peak_working_set_bytes = checked_multiply(
      peak_tile_bytes, static_cast<std::size_t>(actual_workers));
  if (peak_working_set_bytes > request.maximum_transient_bytes) {
    throw_render_error(
        RenderErrorCode::invalid_request,
        "The render worker set exceeds the configured transient memory "
        "budget.");
  }

  const std::size_t output_samples = checked_multiply(
      checked_multiply(
          static_cast<std::size_t>(effective_roi.width),
          static_cast<std::size_t>(effective_roi.height)),
      imaging::ImageF32::channel_count);
  imaging::ImageF32 output{
      .width = effective_roi.width,
      .height = effective_roi.height,
      .encoding = source.encoding,
      .samples = std::vector<float>(output_samples, 0.0F)};

  std::atomic_size_t next_tile{};
  std::atomic_size_t rendered_tiles{};
  std::atomic_size_t cache_hits{};
  std::atomic_size_t cache_misses{};
  std::atomic_bool failed{};
  std::mutex failure_mutex;
  std::exception_ptr failure;

  const auto worker = [&]() {
    while (!failed.load(std::memory_order_relaxed) &&
           !stop_token.stop_requested()) {
      const std::size_t tile_index =
          next_tile.fetch_add(1U, std::memory_order_relaxed);
      if (tile_index >= tile_grid.tile_count) {
        return;
      }
      try {
        const Tile tile = tile_at(tile_grid, tile_index);
        std::optional<std::vector<float>> tile_samples;
        if (cache_ != nullptr && cache_->impl_ != nullptr) {
          const std::string cache_key = make_cache_key(
              prepared.identity,
              prepared.color_encoding,
              tile);
          try {
            tile_samples = cache_->impl_->lookup(cache_key);
          } catch (...) {
            // Cache lookup is best-effort. Allocation failure while copying a
            // cached derivative must not invalidate a render request.
            tile_samples.reset();
          }
          if (tile_samples.has_value()) {
            cache_hits.fetch_add(1U, std::memory_order_relaxed);
          } else {
            cache_misses.fetch_add(1U, std::memory_order_relaxed);
          }
        }
        if (!tile_samples.has_value()) {
          tile_samples = render_tile_samples(
              source, tile, prepared.executable, stop_token);
          rendered_tiles.fetch_add(1U, std::memory_order_relaxed);
        }
        copy_tile_to_output(
            *tile_samples, output, effective_roi, tile);
      } catch (...) {
        failed.store(true, std::memory_order_relaxed);
        std::scoped_lock lock(failure_mutex);
        if (failure == nullptr) {
          failure = std::current_exception();
        }
        return;
      }
    }
  };

  {
    std::vector<std::jthread> workers;
    workers.reserve(actual_workers);
    for (std::uint32_t index = 0; index < actual_workers; ++index) {
      workers.emplace_back(worker);
    }
  }
  if (failure != nullptr) {
    std::rethrow_exception(failure);
  }
  if (stop_token.stop_requested()) {
    throw_render_error(
        RenderErrorCode::cancelled,
        "The render was cancelled before completion.");
  }

  output.validate();
  if (cache_ != nullptr && cache_->impl_ != nullptr) {
    for (std::size_t tile_index = 0U;
         tile_index < tile_grid.tile_count;
         ++tile_index) {
      try {
        const Tile tile = tile_at(tile_grid, tile_index);
        cache_->impl_->store(
            make_cache_key(
                prepared.identity,
                prepared.color_encoding,
                tile),
            copy_tile_from_output(output, effective_roi, tile));
      } catch (...) {
        // Cache admission is a best-effort optimization. A completed render
        // remains valid even if the bounded derivative cache cannot allocate.
      }
    }
  }

  return RenderResult{
      .identity = std::move(prepared.identity),
      .image = std::move(output),
      .diagnostics =
          RenderDiagnostics{
              .rendered_tiles =
                  rendered_tiles.load(std::memory_order_relaxed),
              .cache_hits = cache_hits.load(std::memory_order_relaxed),
              .cache_misses = cache_misses.load(std::memory_order_relaxed),
              .peak_transient_tile_bytes = peak_tile_bytes,
              .peak_transient_working_set_bytes =
                  peak_working_set_bytes,
              .worker_count = actual_workers}};
}

bool is_render_result_current(
    const RenderResult& result,
    const RenderIdentity& expected) noexcept {
  return result.identity == expected;
}

struct RenderPublicationGate::Impl final {
  explicit Impl(RenderIdentity initial)
      : expected_identity(std::move(initial)) {}

  mutable std::mutex mutex;
  RenderIdentity expected_identity;
  std::shared_ptr<const RenderResult> published;
};

RenderPublicationGate::RenderPublicationGate(RenderIdentity expected) {
  validate_render_identity(expected);
  impl_ = std::make_unique<Impl>(std::move(expected));
}

RenderPublicationGate::~RenderPublicationGate() = default;
RenderPublicationGate::RenderPublicationGate(
    RenderPublicationGate&&) noexcept = default;
RenderPublicationGate& RenderPublicationGate::operator=(
    RenderPublicationGate&&) noexcept = default;

void RenderPublicationGate::expect(RenderIdentity expected) {
  validate_render_identity(expected);
  if (impl_ == nullptr) {
    throw std::logic_error{
        "A moved-from render publication gate cannot be reused."};
  }
  std::scoped_lock lock(impl_->mutex);
  impl_->expected_identity = std::move(expected);
  impl_->published.reset();
}

bool RenderPublicationGate::try_publish(RenderResult result) {
  if (impl_ == nullptr) {
    throw std::logic_error{
        "A moved-from render publication gate cannot publish."};
  }
  auto candidate =
      std::make_shared<const RenderResult>(std::move(result));
  std::scoped_lock lock(impl_->mutex);
  if (!is_render_result_current(
          *candidate, impl_->expected_identity)) {
    return false;
  }
  impl_->published = std::move(candidate);
  return true;
}

RenderIdentity RenderPublicationGate::expected() const {
  if (impl_ == nullptr) {
    throw std::logic_error{
        "A moved-from render publication gate has no identity."};
  }
  std::scoped_lock lock(impl_->mutex);
  return impl_->expected_identity;
}

std::shared_ptr<const RenderResult>
RenderPublicationGate::current() const {
  if (impl_ == nullptr) {
    return nullptr;
  }
  std::scoped_lock lock(impl_->mutex);
  return impl_->published;
}

}  // namespace nps::render
