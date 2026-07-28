#include "nps/color/color_encoding.hpp"
#include "nps/document/edit_graph.hpp"
#include "nps/imaging/image_f32.hpp"
#include "nps/imaging/mask16.hpp"
#include "nps/render/cpu_renderer.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace {

using nps::color::ColorEncoding;
using nps::document::CurveControlPoint;
using nps::document::EditGraph;
using nps::document::EditGraphDefinition;
using nps::document::EditNode;
using nps::document::EditNodeKind;
using nps::document::ExposureParameters;
using nps::document::MaskBinding;
using nps::document::OutputParameters;
using nps::document::RgbCurveParameters;
using nps::document::SourceParameters;
using nps::imaging::ImageF32;
using nps::imaging::Mask16;
using nps::render::CancellationSource;
using nps::render::CancellationToken;
using nps::render::CpuRenderer;
using nps::render::CpuTileCache;
using nps::render::ImageRect;
using nps::render::MaskAssetView;
using nps::render::RenderError;
using nps::render::RenderErrorCode;
using nps::render::RenderQuality;
using nps::render::RenderRequest;
using nps::render::RenderResult;
using nps::render::RenderIdentity;
using nps::render::RenderPublicationGate;

constexpr auto linear_rec2020 =
    ColorEncoding::scene_linear_rec2020_d65;
constexpr auto linear_srgb = ColorEncoding::scene_linear_srgb_d65;

class JoiningThreadGroup final {
 public:
  explicit JoiningThreadGroup(const std::size_t capacity) {
    threads_.reserve(capacity);
  }

  ~JoiningThreadGroup() {
    for (auto& thread : threads_) {
      if (thread.joinable()) {
        thread.join();
      }
    }
  }

  JoiningThreadGroup(const JoiningThreadGroup&) = delete;
  JoiningThreadGroup& operator=(const JoiningThreadGroup&) = delete;

  template <typename Callable>
  void start(Callable&& callable) {
    threads_.emplace_back(std::forward<Callable>(callable));
  }

 private:
  std::vector<std::thread> threads_;
};

[[nodiscard]] std::string fake_sha256(const char digit = 'a') {
  return std::string(64U, digit);
}

[[nodiscard]] ImageF32 make_pattern_image(
    const std::uint32_t width,
    const std::uint32_t height,
    const ColorEncoding encoding = linear_rec2020) {
  constexpr std::array<float, 4> alpha_levels{
      0.0F, 0.25F, 0.5F, 1.0F};
  std::vector<float> samples;
  samples.reserve(
      static_cast<std::size_t>(width) * height *
      ImageF32::channel_count);

  for (std::uint32_t y = 0; y < height; ++y) {
    for (std::uint32_t x = 0; x < width; ++x) {
      const float alpha =
          alpha_levels[static_cast<std::size_t>((x + 2U * y) % 4U)];
      if (alpha == 0.0F) {
        samples.insert(samples.end(), {0.0F, 0.0F, 0.0F, 0.0F});
        continue;
      }

      const float straight_red =
          static_cast<float>(static_cast<int>(x % 7U) - 3) * 0.35F;
      const float straight_green =
          static_cast<float>(static_cast<int>(y % 5U) - 2) * 0.4F;
      const float straight_blue =
          static_cast<float>((3U * x + 5U * y) % 11U) * 0.23F - 0.7F;
      samples.insert(
          samples.end(),
          {
              straight_red * alpha,
              straight_green * alpha,
              straight_blue * alpha,
              alpha,
          });
    }
  }

  ImageF32 image{
      .width = width,
      .height = height,
      .encoding = encoding,
      .samples = std::move(samples),
  };
  image.validate();
  return image;
}

[[nodiscard]] ImageF32 make_opaque_image(
    const std::uint32_t width,
    const std::uint32_t height) {
  std::vector<float> samples(
      static_cast<std::size_t>(width) * height *
          ImageF32::channel_count,
      0.0F);
  for (std::size_t index = 0U; index < samples.size();
       index += ImageF32::channel_count) {
    samples[index] = 0.125F;
    samples[index + 1U] = 0.5F;
    samples[index + 2U] = 1.25F;
    samples[index + 3U] = 1.0F;
  }
  return ImageF32{
      .width = width,
      .height = height,
      .encoding = linear_rec2020,
      .samples = std::move(samples),
  };
}

[[nodiscard]] EditNode exposure_node(
    std::string node_id,
    const double ev,
    const bool enabled = true,
    const double opacity = 1.0,
    std::optional<MaskBinding> mask = std::nullopt) {
  EditNode node;
  node.node_id = std::move(node_id);
  node.kind = EditNodeKind::adjust_exposure;
  node.enabled = enabled;
  node.opacity = opacity;
  node.parameters = ExposureParameters{.ev = ev};
  node.mask = std::move(mask);
  return node;
}

[[nodiscard]] EditNode curve_node(
    std::string node_id,
    std::vector<CurveControlPoint> points,
    const bool enabled = true,
    const double opacity = 1.0,
    std::optional<MaskBinding> mask = std::nullopt) {
  EditNode node;
  node.node_id = std::move(node_id);
  node.kind = EditNodeKind::adjust_curve_rgb;
  node.enabled = enabled;
  node.opacity = opacity;
  node.parameters = RgbCurveParameters{.points = std::move(points)};
  node.mask = std::move(mask);
  return node;
}

[[nodiscard]] EditGraph make_graph(
    std::vector<EditNode> adjustments,
    std::string working_color_space =
        std::string(
            nps::document::kSceneLinearRec2020D65WorkingColorSpace)) {
  EditNode source;
  source.node_id = "source";
  source.kind = EditNodeKind::source;
  source.parameters = SourceParameters{};

  std::vector<EditNode> nodes;
  nodes.reserve(adjustments.size() + 2U);
  nodes.push_back(std::move(source));

  std::string previous = "source";
  for (EditNode& adjustment : adjustments) {
    adjustment.inputs = {previous};
    previous = adjustment.node_id;
    nodes.push_back(std::move(adjustment));
  }

  EditNode output;
  output.node_id = "output";
  output.kind = EditNodeKind::output;
  output.inputs = {previous};
  output.parameters = OutputParameters{};
  nodes.push_back(std::move(output));

  auto result = nps::document::create_edit_graph(EditGraphDefinition{
      .graph_id = "renderer-test-graph",
      .working_color_space = std::move(working_color_space),
      .source_node_id = "source",
      .output_node_id = "output",
      .nodes = std::move(nodes),
  });
  if (auto* graph = std::get_if<EditGraph>(&result)) {
    return std::move(*graph);
  }
  throw std::logic_error(
      "A CpuRenderer test constructed an invalid edit graph: " +
      std::get<nps::document::EditGraphError>(result).message);
}

[[nodiscard]] RenderRequest request_for(
    const ImageF32& source,
    const ImageRect roi,
    const std::uint32_t tile_size = 512U,
    const std::uint32_t worker_count = 1U) {
  return RenderRequest{
      .document_id = "renderer-test-document",
      .snapshot_id = "snapshot-17",
      .revision = 17,
      .source_hash = nps::imaging::image_f32_sha256(source),
      .generation = 17U,
      .roi = roi,
      .quality = RenderQuality::final,
      .tile_size = tile_size,
      .worker_count = worker_count,
      .maximum_output_bytes =
          static_cast<std::size_t>(source.width) * source.height *
          ImageF32::channel_count * sizeof(float),
      .maximum_transient_bytes =
          static_cast<std::size_t>(512) * 1024U * 1024U,
  };
}

[[nodiscard]] RenderRequest request_for(
    const ImageF32& source,
    const std::uint32_t tile_size = 512U,
    const std::uint32_t worker_count = 1U) {
  return request_for(
      source,
      ImageRect{
          .x = 0U,
          .y = 0U,
          .width = source.width,
          .height = source.height,
      },
      tile_size,
      worker_count);
}

[[nodiscard]] RenderResult render_without_masks(
    const ImageF32& source,
    const EditGraph& graph,
    const RenderRequest& request,
    const CancellationToken cancellation = {}) {
  return CpuRenderer{}.render(
      source,
      graph,
      std::span<const MaskAssetView>{},
      request,
      cancellation);
}

[[nodiscard]] RenderResult render_with_mask(
    const ImageF32& source,
    const EditGraph& graph,
    const RenderRequest& request,
    const Mask16& mask,
    const std::string_view mask_id = "selection") {
  const std::array views{
      MaskAssetView{
          .mask_id = std::string(mask_id),
          .content_hash = nps::imaging::mask16_sha256(mask),
          .mask = &mask},
  };
  return CpuRenderer{}.render(source, graph, views, request);
}

template <typename Callable>
void require_render_error(
    Callable&& callable,
    const RenderErrorCode expected_code) {
  bool caught = false;
  try {
    static_cast<void>(std::forward<Callable>(callable)());
  } catch (const RenderError& error) {
    caught = true;
    CHECK(error.code() == expected_code);
  }
  REQUIRE(caught);
}

[[nodiscard]] ImageF32 crop_image(
    const ImageF32& source,
    const ImageRect roi) {
  std::vector<float> samples;
  samples.reserve(
      static_cast<std::size_t>(roi.width) * roi.height *
      ImageF32::channel_count);
  for (std::uint32_t y = 0; y < roi.height; ++y) {
    for (std::uint32_t x = 0; x < roi.width; ++x) {
      const auto pixel = source.pixel_span(roi.x + x, roi.y + y);
      samples.insert(samples.end(), pixel.begin(), pixel.end());
    }
  }
  return ImageF32{
      .width = roi.width,
      .height = roi.height,
      .encoding = source.encoding,
      .samples = std::move(samples),
  };
}

void check_pixel(
    const ImageF32& image,
    const std::uint32_t x,
    const std::uint32_t y,
    const std::array<float, 4>& expected) {
  const auto actual = image.pixel_span(x, y);
  for (std::size_t channel = 0U; channel < expected.size(); ++channel) {
    CAPTURE(x, y, channel);
    CHECK(
        actual[channel] ==
        Catch::Approx(expected[channel]).epsilon(0.000001).margin(0.000001));
  }
}

[[nodiscard]] std::size_t tile_count(
    const ImageRect roi,
    const std::uint32_t tile_size) {
  const std::size_t columns =
      (static_cast<std::size_t>(roi.width) + tile_size - 1U) / tile_size;
  const std::size_t rows =
      (static_cast<std::size_t>(roi.height) + tile_size - 1U) / tile_size;
  return columns * rows;
}

}  // namespace

TEST_CASE(
    "CpuRenderer is bit exact across full and odd ROIs, tiles, workers, "
    "and quality labels") {
  const ImageF32 source = make_pattern_image(7U, 5U);
  const EditGraph graph = make_graph(
      {
          exposure_node("exposure-a", 0.75),
          curve_node(
              "curve",
              {
                  {0.0, 0.0},
                  {0.25, 0.1},
                  {0.75, 0.9},
                  {1.0, 1.0},
              }),
          exposure_node("exposure-b", -0.25),
      });

  const RenderRequest full_request = request_for(source);
  const RenderResult full_baseline =
      render_without_masks(source, graph, full_request);
  REQUIRE(full_baseline.image.width == source.width);
  REQUIRE(full_baseline.image.height == source.height);
  CHECK(full_baseline.identity.roi == full_request.roi);
  CHECK(full_baseline.identity.revision == full_request.revision);
  CHECK(
      full_baseline.identity.graph_hash ==
      nps::document::edit_graph_sha256(graph));

  const ImageRect odd_roi{
      .x = 1U,
      .y = 1U,
      .width = 5U,
      .height = 3U,
  };
  const RenderResult roi_baseline = render_without_masks(
      source, graph, request_for(source, odd_roi));
  CHECK(roi_baseline.image == crop_image(full_baseline.image, odd_roi));

  constexpr std::array tile_sizes{1U, 2U, 512U};
  constexpr std::array worker_counts{1U, 4U};
  for (const std::uint32_t tile_size : tile_sizes) {
    for (const std::uint32_t worker_count : worker_counts) {
      CAPTURE(tile_size, worker_count);

      const RenderRequest tiled_full =
          request_for(source, tile_size, worker_count);
      const RenderResult full_candidate =
          render_without_masks(source, graph, tiled_full);
      CHECK(full_candidate.image == full_baseline.image);
      CHECK(
          full_candidate.diagnostics.rendered_tiles ==
          tile_count(full_candidate.identity.roi, tile_size));

      const RenderRequest tiled_roi =
          request_for(source, odd_roi, tile_size, worker_count);
      const RenderResult roi_candidate =
          render_without_masks(source, graph, tiled_roi);
      CHECK(roi_candidate.image == roi_baseline.image);
      CHECK(
          roi_candidate.diagnostics.rendered_tiles ==
          tile_count(odd_roi, tile_size));
    }
  }

  for (const RenderQuality quality : {
           RenderQuality::draft,
           RenderQuality::interactive,
           RenderQuality::final,
       }) {
    RenderRequest quality_request = request_for(source, 2U, 4U);
    quality_request.quality = quality;
    const RenderResult candidate =
        render_without_masks(source, graph, quality_request);
    CHECK(candidate.image == full_baseline.image);
    CHECK(candidate.identity.quality == quality);
  }
}

TEST_CASE(
    "CpuRenderer preserves the canonical 512 boundary and one-pixel "
    "edge tiles") {
  const ImageF32 source = make_pattern_image(513U, 513U);
  const EditGraph graph = make_graph(
      {
          exposure_node("exposure", 0.5),
          curve_node(
              "curve",
              {
                  {0.0, 0.0},
                  {0.25, 0.125},
                  {1.0, 1.0},
              }),
      });
  const RenderResult full = render_without_masks(
      source, graph, request_for(source, 512U, 4U));
  REQUIRE(full.diagnostics.rendered_tiles == 4U);

  constexpr std::array canonical_tiles{
      ImageRect{.x = 0U, .y = 0U, .width = 512U, .height = 512U},
      ImageRect{.x = 512U, .y = 0U, .width = 1U, .height = 512U},
      ImageRect{.x = 0U, .y = 512U, .width = 512U, .height = 1U},
      ImageRect{.x = 512U, .y = 512U, .width = 1U, .height = 1U},
      ImageRect{.x = 511U, .y = 511U, .width = 2U, .height = 2U},
  };
  for (const ImageRect roi : canonical_tiles) {
    CAPTURE(roi.x, roi.y, roi.width, roi.height);
    const RenderResult candidate = render_without_masks(
        source, graph, request_for(source, roi, 512U, 4U));
    CHECK(candidate.image == crop_image(full.image, roi));
    CHECK(
        candidate.diagnostics.rendered_tiles ==
        tile_count(roi, 512U));
  }
}

TEST_CASE(
    "CpuRenderer repeats a fixed-seed legal graph and ROI corpus bit "
    "exactly across schedules") {
  const ImageF32 source = make_pattern_image(31U, 23U);
  std::uint32_t state = 0x4E50534DU;
  const auto next_value = [&state]() {
    state = state * 1664525U + 1013904223U;
    return state;
  };

  constexpr std::size_t graph_count = 12U;
  constexpr std::size_t rois_per_graph = 8U;
  for (std::size_t graph_index = 0U;
       graph_index < graph_count;
       ++graph_index) {
    const std::size_t node_count =
        1U + static_cast<std::size_t>(next_value() % 6U);
    std::vector<EditNode> nodes;
    nodes.reserve(node_count);
    for (std::size_t node_index = 0U;
         node_index < node_count;
         ++node_index) {
      const std::string node_id =
          "corpus-" + std::to_string(graph_index) + "-" +
          std::to_string(node_index);
      if ((next_value() & 1U) == 0U) {
        const double ev =
            static_cast<double>(
                static_cast<std::int32_t>(next_value() % 17U) - 8) /
            4.0;
        nodes.push_back(exposure_node(node_id, ev));
      } else {
        const auto ordinate = [&next_value]() {
          return static_cast<double>(next_value() % 17U) / 16.0;
        };
        nodes.push_back(curve_node(
            node_id,
            {
                {0.0, ordinate()},
                {0.5, ordinate()},
                {1.0, ordinate()},
            }));
      }
    }
    const EditGraph graph = make_graph(std::move(nodes));

    for (std::size_t roi_index = 0U;
         roi_index < rois_per_graph;
         ++roi_index) {
      const std::uint32_t x = next_value() % source.width;
      const std::uint32_t y = next_value() % source.height;
      const ImageRect roi{
          .x = x,
          .y = y,
          .width = 1U + next_value() % (source.width - x),
          .height = 1U + next_value() % (source.height - y),
      };
      CAPTURE(graph_index, roi_index, roi.x, roi.y, roi.width, roi.height);

      const RenderResult serial = render_without_masks(
          source, graph, request_for(source, roi, 7U, 1U));
      const RenderRequest parallel_request =
          request_for(source, roi, 3U, 4U);
      const RenderResult parallel =
          render_without_masks(source, graph, parallel_request);
      const RenderResult repeated =
          render_without_masks(source, graph, parallel_request);
      CHECK(parallel.image == serial.image);
      CHECK(repeated.image == parallel.image);
      CHECK(parallel.identity == repeated.identity);
    }
  }
}

TEST_CASE(
    "CpuRenderer composite golden covers exposure curve and Mask16 "
    "coverage endpoints") {
  const ImageF32 source{
      .width = 4U,
      .height = 1U,
      .encoding = linear_rec2020,
      .samples = {
          0.25F, -0.5F, 2.0F, 1.0F,
          0.125F, -0.25F, 0.5F, 0.5F,
          -0.5F, 0.25F, 1.0F, 1.0F,
          0.0625F, -0.125F, 0.25F, 0.25F,
      },
  };
  const Mask16 mask{
      .width = 4U,
      .height = 1U,
      .samples = {0U, 1U, 32768U, Mask16::maximum},
  };
  const EditGraph graph = make_graph(
      {
          exposure_node("exposure", 1.0),
          curve_node(
              "masked-curve",
              {
                  {0.0, 0.0},
                  {1.0, 0.5},
              },
              true,
              1.0,
              MaskBinding{
                  .mask_id = "selection",
                  .content_hash = nps::imaging::mask16_sha256(mask),
                  .inverted = false}),
      });

  const ImageF32 result =
      render_with_mask(source, graph, request_for(source), mask).image;
  check_pixel(result, 0U, 0U, {0.5F, -1.0F, 4.0F, 1.0F});
  check_pixel(
      result,
      1U,
      0U,
      {0.249998093F, -0.499996185F, 0.999992371F, 0.5F});
  check_pixel(
      result,
      2U,
      0U,
      {-0.749996185F, 0.374998093F, 1.499992371F, 1.0F});
  check_pixel(
      result,
      3U,
      0U,
      {0.0625F, -0.125F, 0.25F, 0.25F});
}

TEST_CASE(
    "CpuRenderer applies zero, full, gradient, and inverted masks "
    "without quantization ambiguity") {
  constexpr std::array<float, 4> source_pixel{
      0.25F, -0.5F, 2.0F, 1.0F};
  std::vector<float> samples;
  for (std::size_t pixel = 0U; pixel < 4U; ++pixel) {
    samples.insert(samples.end(), source_pixel.begin(), source_pixel.end());
  }
  const ImageF32 source{
      .width = 4U,
      .height = 1U,
      .encoding = linear_rec2020,
      .samples = std::move(samples),
  };
  const RenderRequest request = request_for(source, 2U, 4U);
  const auto graph_for = [](const Mask16& mask, const bool inverted) {
    return make_graph(
        {exposure_node(
            "masked-exposure",
            1.0,
            true,
            1.0,
            MaskBinding{
                .mask_id = "selection",
                .content_hash =
                    nps::imaging::mask16_sha256(mask),
                .inverted = inverted})});
  };

  const Mask16 zero_mask{4U, 1U, {0U, 0U, 0U, 0U}};
  CHECK(
      render_with_mask(
          source, graph_for(zero_mask, false), request, zero_mask)
              .image ==
      source);

  const Mask16 full_mask{
      4U,
      1U,
      {
          Mask16::maximum,
          Mask16::maximum,
          Mask16::maximum,
          Mask16::maximum,
      }};
  const ImageF32 full_result =
      render_with_mask(
          source, graph_for(full_mask, false), request, full_mask)
          .image;
  for (std::uint32_t x = 0U; x < source.width; ++x) {
    check_pixel(
        full_result,
        x,
        0U,
        {
            source_pixel[0] * 2.0F,
            source_pixel[1] * 2.0F,
            source_pixel[2] * 2.0F,
            1.0F,
        });
  }

  const Mask16 gradient_mask{
      4U,
      1U,
      {0U, 16384U, 32768U, Mask16::maximum}};
  const ImageF32 gradient_result =
      render_with_mask(
          source,
          graph_for(gradient_mask, false),
          request,
          gradient_mask)
          .image;
  const ImageF32 inverse_result =
      render_with_mask(
          source,
          graph_for(gradient_mask, true),
          request,
          gradient_mask)
          .image;
  for (std::uint32_t x = 0U; x < source.width; ++x) {
    const double coverage =
        static_cast<double>(gradient_mask.samples[x]) /
        static_cast<double>(Mask16::maximum);
    std::array<float, 4> expected_normal{};
    std::array<float, 4> expected_inverse{};
    for (std::size_t channel = 0U; channel < 3U; ++channel) {
      const double original = source_pixel[channel];
      expected_normal[channel] =
          static_cast<float>(original + original * coverage);
      expected_inverse[channel] =
          static_cast<float>(original + original * (1.0 - coverage));
    }
    expected_normal[3] = 1.0F;
    expected_inverse[3] = 1.0F;
    check_pixel(gradient_result, x, 0U, expected_normal);
    check_pixel(inverse_result, x, 0U, expected_inverse);
  }
}

TEST_CASE("CpuRenderer honors disabled nodes and opacity before publication") {
  const ImageF32 source{
      .width = 1U,
      .height = 1U,
      .encoding = linear_rec2020,
      .samples = {0.5F, -0.25F, 1.25F, 0.5F},
  };
  const RenderRequest request = request_for(source);

  const EditGraph disabled =
      make_graph({exposure_node("exposure", 2.0, false, 0.75)});
  CHECK(render_without_masks(source, disabled, request).image == source);

  const EditGraph zero_opacity =
      make_graph({exposure_node("exposure", 2.0, true, 0.0)});
  CHECK(render_without_masks(source, zero_opacity, request).image == source);

  const EditGraph partial =
      make_graph({exposure_node("exposure", 2.0, true, 0.25)});
  const ImageF32 partial_result =
      render_without_masks(source, partial, request).image;
  check_pixel(
      partial_result,
      0U,
      0U,
      {0.875F, -0.4375F, 2.1875F, 0.5F});

  const EditGraph passthrough = make_graph({});
  CHECK(render_without_masks(source, passthrough, request).image == source);
}

TEST_CASE(
    "CpuRenderer exposure retains negative and HDR scene values at EV "
    "boundaries") {
  const ImageF32 source{
      .width = 1U,
      .height = 1U,
      .encoding = linear_rec2020,
      .samples = {-0.25F, 2.0F, 0.75F, 0.5F},
  };
  const RenderRequest request = request_for(source);

  const EditGraph maximum =
      make_graph({exposure_node("maximum-exposure", 10.0)});
  const ImageF32 maximum_result =
      render_without_masks(source, maximum, request).image;
  check_pixel(
      maximum_result,
      0U,
      0U,
      {-256.0F, 2048.0F, 768.0F, 0.5F});
  CHECK(maximum_result.samples[0] < 0.0F);
  CHECK(maximum_result.samples[1] > 1.0F);

  const EditGraph round_trip = make_graph(
      {
          exposure_node("increase", 10.0),
          exposure_node("decrease", -10.0),
      });
  CHECK(render_without_masks(source, round_trip, request).image == source);

  const ImageF32 extreme{
      .width = 1U,
      .height = 1U,
      .encoding = linear_rec2020,
      .samples = {
          std::numeric_limits<float>::max(),
          0.0F,
          0.0F,
          1.0F,
      },
  };
  require_render_error(
      [&] {
        return render_without_masks(
            extreme, maximum, request_for(extreme));
      },
      RenderErrorCode::numeric_failure);
}

TEST_CASE(
    "CpuRenderer evaluates RGB curves in straight color and never creates "
    "hidden transparent RGB") {
  const ImageF32 source{
      .width = 3U,
      .height = 1U,
      .encoding = linear_rec2020,
      .samples = {
          0.0F,
          0.0F,
          0.0F,
          0.0F,
          0.125F,
          0.25F,
          -0.25F,
          0.25F,
          1.0F,
          0.125F,
          0.0F,
          0.5F,
      },
  };
  const EditGraph graph = make_graph(
      {curve_node(
          "curve",
          {
              {0.0, 0.25},
              {1.0, 0.75},
          })});
  const ImageF32 result =
      render_without_masks(source, graph, request_for(source)).image;

  check_pixel(result, 0U, 0U, {0.0F, 0.0F, 0.0F, 0.0F});
  check_pixel(result, 1U, 0U, {0.125F, 0.1875F, -0.0625F, 0.25F});
  check_pixel(result, 2U, 0U, {0.625F, 0.1875F, 0.125F, 0.5F});
  REQUIRE_NOTHROW(result.validate());
}

TEST_CASE(
    "CpuRenderer distinguishes missing, wrong-size, and duplicate masks") {
  const ImageF32 source = make_pattern_image(3U, 2U);
  const Mask16 valid{
      .width = source.width,
      .height = source.height,
      .samples = std::vector<std::uint16_t>(
          static_cast<std::size_t>(source.width) * source.height,
          Mask16::maximum),
  };
  const EditGraph graph = make_graph(
      {exposure_node(
          "masked",
          1.0,
          true,
          1.0,
          MaskBinding{
              .mask_id = "selection",
              .content_hash = nps::imaging::mask16_sha256(valid),
              .inverted = false})});
  const RenderRequest request = request_for(source);

  require_render_error(
      [&] {
        return render_without_masks(source, graph, request);
      },
      RenderErrorCode::missing_mask);

  const Mask16 wrong_size{
      .width = 2U,
      .height = 2U,
      .samples = std::vector<std::uint16_t>(4U, Mask16::maximum),
  };
  require_render_error(
      [&] {
        const std::array views{
            MaskAssetView{
                .mask_id = "selection",
                .content_hash =
                    nps::imaging::mask16_sha256(wrong_size),
                .mask = &wrong_size,
            },
        };
        return CpuRenderer{}.render(source, graph, views, request);
      },
      RenderErrorCode::mask_dimension_mismatch);

  require_render_error(
      [&] {
        const std::array duplicate_views{
            MaskAssetView{
                .mask_id = "selection",
                .content_hash =
                    nps::imaging::mask16_sha256(valid),
                .mask = &valid,
            },
            MaskAssetView{
                .mask_id = "selection",
                .content_hash =
                    nps::imaging::mask16_sha256(valid),
                .mask = &valid,
            },
        };
        return CpuRenderer{}.render(
            source, graph, duplicate_views, request);
      },
      RenderErrorCode::invalid_request);

  require_render_error(
      [&] {
        const std::array null_view{
            MaskAssetView{
                .mask_id = "selection",
                .content_hash =
                    nps::imaging::mask16_sha256(valid),
                .mask = nullptr,
            },
        };
        return CpuRenderer{}.render(source, graph, null_view, request);
      },
      RenderErrorCode::invalid_request);

  require_render_error(
      [&] {
        const std::array invalid_hash_view{
            MaskAssetView{
                .mask_id = "selection",
                .content_hash = "not-a-sha256",
                .mask = &valid,
            },
        };
        return CpuRenderer{}.render(
            source, graph, invalid_hash_view, request);
      },
      RenderErrorCode::invalid_request);

  require_render_error(
      [&] {
        const std::array false_hash_view{
            MaskAssetView{
                .mask_id = "selection",
                .content_hash = fake_sha256('c'),
                .mask = &valid,
            },
        };
        return CpuRenderer{}.render(
            source, graph, false_hash_view, request);
      },
      RenderErrorCode::invalid_request);
}

TEST_CASE(
    "CpuRenderer rejects malformed requests, hashes, and output budgets") {
  const ImageF32 source = make_pattern_image(3U, 2U);
  const EditGraph graph = make_graph({});
  const RenderRequest valid = request_for(source);
  const std::size_t exact_output_bytes =
      static_cast<std::size_t>(source.width) * source.height *
      ImageF32::channel_count * sizeof(float);

  RenderRequest exact_budget = valid;
  exact_budget.maximum_output_bytes = exact_output_bytes;
  REQUIRE_NOTHROW(render_without_masks(source, graph, exact_budget));

  const auto expect_invalid = [&](RenderRequest request) {
    require_render_error(
        [&] {
          return render_without_masks(source, graph, request);
        },
        RenderErrorCode::invalid_request);
  };

  SECTION("missing or malformed immutable target identities") {
    for (const bool clear_document : {false, true}) {
      RenderRequest request = valid;
      if (clear_document) {
        request.document_id.clear();
      } else {
        request.snapshot_id = "../snapshot";
      }
      expect_invalid(std::move(request));
    }
  }
  SECTION("negative revision") {
    RenderRequest request = valid;
    request.revision = -1;
    expect_invalid(std::move(request));
  }
  SECTION("non SHA-256 source identities") {
    for (std::string invalid_hash : {
             std::string{},
             std::string(63U, 'a'),
             std::string(64U, 'A'),
             std::string(64U, 'g'),
         }) {
      CAPTURE(invalid_hash.size());
      RenderRequest request = valid;
      request.source_hash = std::move(invalid_hash);
      expect_invalid(std::move(request));
    }
  }
  SECTION("well-formed but false source content identity") {
    RenderRequest request = valid;
    request.source_hash = fake_sha256('b');
    expect_invalid(std::move(request));
  }
  SECTION("empty ROI") {
    RenderRequest request = valid;
    request.roi.width = 0U;
    expect_invalid(std::move(request));
  }
  SECTION("partially outside ROI is clipped to the source") {
    RenderRequest request = valid;
    request.roi = ImageRect{.x = 2U, .y = 0U, .width = 2U, .height = 1U};
    const RenderResult clipped =
        render_without_masks(source, graph, request);
    CHECK((
        clipped.identity.roi ==
        ImageRect{.x = 2U, .y = 0U, .width = 1U, .height = 1U}));
    CHECK(clipped.image.width == 1U);
    CHECK(clipped.image.height == 1U);
  }
  SECTION("fully outside ROI has a stable empty-region result") {
    RenderRequest request = valid;
    request.roi = ImageRect{.x = 3U, .y = 0U, .width = 1U, .height = 1U};
    require_render_error(
        [&] {
          return render_without_masks(source, graph, request);
        },
        RenderErrorCode::empty_region);
  }
  SECTION("ROI coordinate addition cannot wrap") {
    RenderRequest request = valid;
    request.roi = ImageRect{
        .x = std::numeric_limits<std::uint32_t>::max(),
        .y = 0U,
        .width = 1U,
        .height = 1U,
    };
    require_render_error(
        [&] {
          return render_without_masks(source, graph, request);
        },
        RenderErrorCode::empty_region);
  }
  SECTION("zero tile size") {
    RenderRequest request = valid;
    request.tile_size = 0U;
    expect_invalid(std::move(request));
  }
  SECTION("oversized tile") {
    RenderRequest request = valid;
    request.tile_size = 513U;
    expect_invalid(std::move(request));
  }
  SECTION("zero workers") {
    RenderRequest request = valid;
    request.worker_count = 0U;
    expect_invalid(std::move(request));
  }
  SECTION("too many workers") {
    RenderRequest request = valid;
    request.worker_count = 65U;
    expect_invalid(std::move(request));
  }
  SECTION("output budget one byte below the exact requirement") {
    RenderRequest request = valid;
    request.maximum_output_bytes = exact_output_bytes - 1U;
    expect_invalid(std::move(request));
  }
  SECTION("transient worker budget is enforced exactly") {
    RenderRequest exact = valid;
    exact.maximum_transient_bytes = exact_output_bytes;
    REQUIRE_NOTHROW(render_without_masks(source, graph, exact));

    RenderRequest insufficient = valid;
    insufficient.maximum_transient_bytes = exact_output_bytes - 1U;
    expect_invalid(std::move(insufficient));
  }
  SECTION("invalid quality enumerator") {
    RenderRequest request = valid;
    request.quality = static_cast<RenderQuality>(255);
    expect_invalid(std::move(request));
  }
}

TEST_CASE("CpuRenderer rejects an unbounded diagnostic tile grid") {
  const ImageF32 source = make_pattern_image(257U, 257U);
  const EditGraph graph = make_graph({});
  RenderRequest request = request_for(source, 1U);
  REQUIRE(
      tile_count(request.roi, request.tile_size) >
      nps::render::kMaximumTilesPerRenderRequest);

  require_render_error(
      [&] {
        return render_without_masks(source, graph, request);
      },
      RenderErrorCode::invalid_request);
  require_render_error(
      [&] {
        return nps::render::make_render_identity(
            source,
            graph,
            std::span<const MaskAssetView>{},
            request);
      },
      RenderErrorCode::invalid_request);

  auto cache = std::make_shared<CpuTileCache>(4U * 1024U * 1024U);
  require_render_error(
      [&] {
        return CpuRenderer{cache}.render(
            source,
            graph,
            std::span<const MaskAssetView>{},
            request);
      },
      RenderErrorCode::invalid_request);
  CHECK(cache->stats().entry_count == 0U);
  CHECK(cache->stats().resident_bytes == 0U);
}

TEST_CASE("CpuRenderer rejects a graph and source color-space mismatch") {
  const ImageF32 source = make_pattern_image(2U, 2U, linear_srgb);
  const EditGraph rec2020_graph = make_graph({});
  require_render_error(
      [&] {
        return render_without_masks(
            source, rec2020_graph, request_for(source));
      },
      RenderErrorCode::unsupported_graph);
}

TEST_CASE("CancellationSource shares one monotonic cancellation state") {
  const CancellationToken empty_token;
  CHECK_FALSE(empty_token.stop_requested());

  CancellationSource source;
  CancellationSource source_copy = source;
  const CancellationToken token = source.get_token();
  const CancellationToken token_copy = token;
  CHECK_FALSE(token.stop_requested());
  CHECK_FALSE(token_copy.stop_requested());

  REQUIRE(source_copy.request_stop());
  CHECK_FALSE(source.request_stop());
  CHECK(token.stop_requested());
  CHECK(token_copy.stop_requested());
}

TEST_CASE("CpuRenderer observes cancellation before and during work") {
  const ImageF32 source = make_opaque_image(64U, 64U);
  const EditGraph simple_graph =
      make_graph({exposure_node("exposure", 1.0)});
  const RenderRequest request = request_for(source, 64U, 1U);

  CancellationSource pre_cancelled;
  REQUIRE(pre_cancelled.request_stop());
  require_render_error(
      [&] {
        return render_without_masks(
            source, simple_graph, request, pre_cancelled.get_token());
      },
      RenderErrorCode::cancelled);

  std::vector<EditNode> long_chain;
  constexpr std::size_t long_chain_nodes = 2048U;
  long_chain.reserve(long_chain_nodes);
  for (std::size_t index = 0U; index < long_chain_nodes; ++index) {
    long_chain.push_back(curve_node(
        "curve-" + std::to_string(index),
        {
            {0.0, 0.0},
            {1.0, 1.0},
        }));
  }
  const EditGraph long_graph = make_graph(std::move(long_chain));

  auto cancellation_cache =
      std::make_shared<CpuTileCache>(4U * 1024U * 1024U);
  const CpuRenderer cached_renderer{cancellation_cache};
  CancellationSource in_flight_stop;
  std::atomic_bool call_entered{false};
  bool caught_cancelled = false;
  RenderErrorCode observed_code = RenderErrorCode::invalid_request;
  std::exception_ptr unexpected_failure;
  {
    JoiningThreadGroup render_threads(1U);
    render_threads.start([&] {
      call_entered.store(true, std::memory_order_release);
      try {
        static_cast<void>(cached_renderer.render(
            source,
            long_graph,
            std::span<const MaskAssetView>{},
            request,
            in_flight_stop.get_token()));
      } catch (const RenderError& error) {
        caught_cancelled = true;
        observed_code = error.code();
      } catch (...) {
        unexpected_failure = std::current_exception();
      }
    });

    while (!call_entered.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    REQUIRE(in_flight_stop.request_stop());
  }

  REQUIRE(unexpected_failure == nullptr);
  REQUIRE(caught_cancelled);
  CHECK(observed_code == RenderErrorCode::cancelled);
  CHECK(cancellation_cache->stats().entry_count == 0U);
  CHECK(cancellation_cache->stats().resident_bytes == 0U);
}

TEST_CASE(
    "CpuRenderer never admits staged tiles when any tile fails") {
  ImageF32 source = make_opaque_image(4U, 2U);
  source.pixel_span(3U, 1U)[0] =
      std::numeric_limits<float>::max();
  source.validate();
  const EditGraph graph =
      make_graph({exposure_node("overflow", 1.0)});
  RenderRequest request = request_for(source, 2U, 2U);
  auto cache = std::make_shared<CpuTileCache>(4096U);
  const CpuRenderer renderer{cache};

  require_render_error(
      [&] {
        return renderer.render(
            source,
            graph,
            std::span<const MaskAssetView>{},
            request);
      },
      RenderErrorCode::numeric_failure);
  CHECK(cache->stats().entry_count == 0U);
  CHECK(cache->stats().resident_bytes == 0U);
}

TEST_CASE(
    "CpuTileCache is bounded and isolates immutable snapshot identities") {
  REQUIRE_THROWS_AS(CpuTileCache{0U}, std::invalid_argument);

  const ImageF32 source = make_pattern_image(4U, 2U);
  const EditGraph graph =
      make_graph({exposure_node("exposure", 0.5)});
  constexpr std::size_t bytes_per_tile =
      2U * 2U * ImageF32::channel_count * sizeof(float);
  constexpr std::size_t tiles_per_render = 2U;
  RenderRequest request = request_for(source, 2U, 2U);
  const auto render_with = [&](
                               const CpuRenderer& candidate_renderer,
                               const RenderRequest& candidate_request) {
    return candidate_renderer.render(
        source,
        graph,
        std::span<const MaskAssetView>{},
        candidate_request);
  };

  // Calibrate the subject budget from this standard-library ABI's own
  // conservative accounting. The product contract is a byte budget, not a
  // fixed number of entries for an arbitrary numeric capacity.
  auto accounting_probe = std::make_shared<CpuTileCache>(
      std::numeric_limits<std::size_t>::max());
  const CpuRenderer probe_renderer{accounting_probe};
  const RenderResult probe_cold = render_with(probe_renderer, request);
  REQUIRE(probe_cold.diagnostics.rendered_tiles == tiles_per_render);
  REQUIRE(probe_cold.diagnostics.cache_hits == 0U);
  REQUIRE(probe_cold.diagnostics.cache_misses == tiles_per_render);
  const auto probe_stats = accounting_probe->stats();
  REQUIRE(probe_stats.entry_count == tiles_per_render);
  REQUIRE(
      probe_stats.resident_bytes > tiles_per_render * bytes_per_tile);
  const std::size_t cache_capacity = probe_stats.resident_bytes;

  auto cache = std::make_shared<CpuTileCache>(cache_capacity);
  const CpuRenderer renderer{cache};
  const auto render = [&](const RenderRequest& candidate) {
    return render_with(renderer, candidate);
  };

  const RenderResult cold = render(request);
  CHECK(cold.image == probe_cold.image);
  CHECK(cold.diagnostics.rendered_tiles == 2U);
  CHECK(cold.diagnostics.cache_hits == 0U);
  CHECK(cold.diagnostics.cache_misses == 2U);
  CHECK(cache->stats().resident_bytes > 2U * bytes_per_tile);
  CHECK(cache->stats().resident_bytes <= cache_capacity);
  CHECK(cache->stats().capacity_bytes == cache_capacity);

  const RenderResult hot = render(request);
  CHECK(hot.image == cold.image);
  CHECK(hot.diagnostics.rendered_tiles == 0U);
  CHECK(hot.diagnostics.cache_hits == 2U);
  CHECK(hot.diagnostics.cache_misses == 0U);

  RenderRequest isolated_request = request;
  isolated_request.snapshot_id = "cache-pressure";
  const RenderResult isolated = render(isolated_request);
  CHECK(isolated.image == cold.image);
  CHECK(isolated.diagnostics.rendered_tiles == tiles_per_render);
  CHECK(isolated.diagnostics.cache_hits == 0U);
  CHECK(isolated.diagnostics.cache_misses == tiles_per_render);
  const RenderResult isolated_hot = render(isolated_request);
  CHECK(isolated_hot.image == isolated.image);
  CHECK(isolated_hot.diagnostics.rendered_tiles == 0U);
  CHECK(isolated_hot.diagnostics.cache_hits == tiles_per_render);
  CHECK(isolated_hot.diagnostics.cache_misses == 0U);

  const auto pressured_stats = cache->stats();
  CHECK(
      pressured_stats.resident_bytes <= pressured_stats.capacity_bytes);

  // The calibrated budget held exactly one two-tile identity before the
  // isolated identity arrived. A fully cold baseline behaviorally proves
  // that bounded LRU pressure evicted both original tile keys.
  const RenderResult evicted_baseline = render(request);
  CHECK(evicted_baseline.image == cold.image);
  CHECK(evicted_baseline.diagnostics.rendered_tiles == tiles_per_render);
  CHECK(evicted_baseline.diagnostics.cache_hits == 0U);
  CHECK(evicted_baseline.diagnostics.cache_misses == tiles_per_render);
  CHECK(cache->stats().resident_bytes <=
        cache->stats().capacity_bytes);

  cache->clear();
  CHECK(cache->stats().entry_count == 0U);
  CHECK(cache->stats().resident_bytes == 0U);
}

TEST_CASE(
    "CpuTileCache isolates every immutable request identity domain") {
  const ImageF32 source = make_pattern_image(2U, 2U);
  const EditGraph graph =
      make_graph({exposure_node("exposure", 0.5)});
  auto cache = std::make_shared<CpuTileCache>(64U * 1024U);
  const CpuRenderer renderer{cache};
  const RenderRequest baseline_request = request_for(source, 2U, 2U);

  const auto render = [&](
                          const ImageF32& candidate_source,
                          const EditGraph& candidate_graph,
                          const RenderRequest& candidate_request) {
    return renderer.render(
        candidate_source,
        candidate_graph,
        std::span<const MaskAssetView>{},
        candidate_request);
  };
  const auto require_cold_then_hot = [&](
                                         const ImageF32& candidate_source,
                                         const EditGraph& candidate_graph,
                                         const RenderRequest&
                                             candidate_request) {
    const RenderResult cold =
        render(candidate_source, candidate_graph, candidate_request);
    CHECK(cold.diagnostics.rendered_tiles == 1U);
    CHECK(cold.diagnostics.cache_hits == 0U);
    CHECK(cold.diagnostics.cache_misses == 1U);
    const RenderResult hot =
        render(candidate_source, candidate_graph, candidate_request);
    CHECK(hot.image == cold.image);
    CHECK(hot.diagnostics.rendered_tiles == 0U);
    CHECK(hot.diagnostics.cache_hits == 1U);
    CHECK(hot.diagnostics.cache_misses == 0U);
  };

  require_cold_then_hot(source, graph, baseline_request);

  RenderRequest candidate = baseline_request;
  candidate.document_id = "renderer-test-document-b";
  require_cold_then_hot(source, graph, candidate);

  candidate = baseline_request;
  candidate.snapshot_id = "snapshot-18";
  require_cold_then_hot(source, graph, candidate);

  candidate = baseline_request;
  ++candidate.revision;
  require_cold_then_hot(source, graph, candidate);

  candidate = baseline_request;
  ++candidate.generation;
  require_cold_then_hot(source, graph, candidate);

  candidate = baseline_request;
  candidate.quality = RenderQuality::draft;
  require_cold_then_hot(source, graph, candidate);

  candidate = baseline_request;
  candidate.roi =
      ImageRect{.x = 1U, .y = 0U, .width = 1U, .height = 2U};
  require_cold_then_hot(source, graph, candidate);

  const EditGraph changed_graph =
      make_graph({exposure_node("exposure", 0.75)});
  require_cold_then_hot(source, changed_graph, baseline_request);

  ImageF32 changed_source = source;
  changed_source.pixel_span(1U, 1U)[0] += 0.03125F;
  changed_source.validate();
  const RenderRequest changed_source_request =
      request_for(changed_source, 2U, 2U);
  require_cold_then_hot(
      changed_source, graph, changed_source_request);

  const Mask16 zero_mask{
      .width = source.width,
      .height = source.height,
      .samples = {0U, 0U, 0U, 0U},
  };
  const Mask16 full_mask{
      .width = source.width,
      .height = source.height,
      .samples = {
          Mask16::maximum,
          Mask16::maximum,
          Mask16::maximum,
          Mask16::maximum,
      },
  };
  const auto masked_graph = [](const Mask16& mask) {
    return make_graph(
        {exposure_node(
            "masked-exposure",
            1.0,
            true,
            1.0,
            MaskBinding{
                .mask_id = "selection",
                .content_hash =
                    nps::imaging::mask16_sha256(mask),
                .inverted = false})});
  };
  const auto require_mask_cold_then_hot = [&](
                                               const Mask16& mask) {
    const EditGraph candidate_graph = masked_graph(mask);
    const std::array views{
        MaskAssetView{
            .mask_id = "selection",
            .content_hash = nps::imaging::mask16_sha256(mask),
            .mask = &mask},
    };
    const RenderResult cold = renderer.render(
        source, candidate_graph, views, baseline_request);
    CHECK(cold.diagnostics.rendered_tiles == 1U);
    CHECK(cold.diagnostics.cache_hits == 0U);
    CHECK(cold.diagnostics.cache_misses == 1U);
    const RenderResult hot = renderer.render(
        source, candidate_graph, views, baseline_request);
    CHECK(hot.image == cold.image);
    CHECK(hot.diagnostics.rendered_tiles == 0U);
    CHECK(hot.diagnostics.cache_hits == 1U);
    CHECK(hot.diagnostics.cache_misses == 0U);
  };
  require_mask_cold_then_hot(zero_mask);
  require_mask_cold_then_hot(full_mask);

  CHECK(cache->stats().resident_bytes <= cache->stats().capacity_bytes);
}

TEST_CASE(
    "render currentness compares every publication identity field") {
  const ImageF32 source = make_pattern_image(2U, 2U);
  const EditGraph graph =
      make_graph({exposure_node("exposure", 0.5)});
  RenderRequest request = request_for(source);
  request.revision = 42;
  request.generation = 99U;
  const RenderResult result =
      render_without_masks(source, graph, request);

  CHECK(
      nps::render::make_render_identity(
          source,
          graph,
          std::span<const MaskAssetView>{},
          request) == result.identity);
  REQUIRE(nps::render::is_render_result_current(
      result, result.identity));

  const auto expect_stale = [&](const RenderIdentity& changed) {
    CHECK_FALSE(
        nps::render::is_render_result_current(result, changed));
  };

  RenderIdentity changed = result.identity;
  changed.document_id = "another-document";
  expect_stale(changed);
  changed = result.identity;
  changed.snapshot_id = "another-snapshot";
  expect_stale(changed);
  changed = result.identity;
  changed.revision = 41;
  expect_stale(changed);
  changed = result.identity;
  changed.source_hash = fake_sha256('b');
  expect_stale(changed);
  changed = result.identity;
  changed.graph_hash = fake_sha256('b');
  expect_stale(changed);
  changed = result.identity;
  changed.mask_signature = fake_sha256('b');
  expect_stale(changed);
  changed = result.identity;
  changed.roi.x = 1U;
  expect_stale(changed);
  changed = result.identity;
  changed.quality = RenderQuality::draft;
  expect_stale(changed);
  changed = result.identity;
  ++changed.generation;
  expect_stale(changed);
}

TEST_CASE(
    "RenderPublicationGate suppresses late ROI, quality, asset, and "
    "generation results") {
  const ImageF32 source = make_pattern_image(4U, 2U);
  const EditGraph graph =
      make_graph({exposure_node("exposure", 0.5)});

  RenderRequest old_request = request_for(
      source,
      ImageRect{.x = 0U, .y = 0U, .width = 2U, .height = 2U});
  old_request.quality = RenderQuality::draft;
  old_request.generation = 100U;
  RenderResult old_result =
      render_without_masks(source, graph, old_request);

  RenderRequest final_request = request_for(source);
  final_request.quality = RenderQuality::final;
  final_request.generation = 101U;
  RenderResult final_result =
      render_without_masks(source, graph, final_request);

  RenderPublicationGate gate{final_result.identity};
  CHECK_FALSE(gate.try_publish(std::move(old_result)));
  CHECK(gate.current() == nullptr);
  REQUIRE(gate.try_publish(std::move(final_result)));
  const auto published = gate.current();
  REQUIRE(published != nullptr);
  CHECK(published->identity.quality == RenderQuality::final);
  CHECK(published->identity.generation == 101U);

  SECTION("changed source content identity invalidates a late result") {
    RenderResult candidate =
        render_without_masks(source, graph, final_request);
    RenderIdentity expected = candidate.identity;
    expected.source_hash = fake_sha256('b');
    ++expected.generation;
    gate.expect(expected);
    CHECK_FALSE(gate.try_publish(std::move(candidate)));
    CHECK(gate.current() == nullptr);
  }

  SECTION("changed mask signature invalidates a late result") {
    RenderResult candidate =
        render_without_masks(source, graph, final_request);
    RenderIdentity expected = candidate.identity;
    expected.mask_signature = fake_sha256('c');
    ++expected.generation;
    gate.expect(expected);
    CHECK_FALSE(gate.try_publish(std::move(candidate)));
    CHECK(gate.current() == nullptr);
  }
}

TEST_CASE(
    "RenderPublicationGate deterministically resolves concurrent late and "
    "current results") {
  const ImageF32 source = make_pattern_image(2U, 2U);
  const EditGraph graph =
      make_graph({exposure_node("exposure", 0.5)});

  RenderRequest late_request = request_for(source);
  late_request.snapshot_id = "snapshot-late";
  late_request.generation = 100U;
  const RenderResult late =
      render_without_masks(source, graph, late_request);

  RenderRequest current_request = request_for(source);
  current_request.snapshot_id = "snapshot-current";
  current_request.generation = 101U;
  const RenderResult current =
      render_without_masks(source, graph, current_request);

  RenderPublicationGate gate{current.identity};
  constexpr std::size_t race_repetitions = 32U;
  for (std::size_t repetition = 0U;
       repetition < race_repetitions;
       ++repetition) {
    CAPTURE(repetition);
    gate.expect(current.identity);

    std::atomic_uint32_t ready{};
    std::atomic_bool start{};
    std::atomic_uint32_t accepted{};
    std::mutex failure_mutex;
    std::exception_ptr unexpected_failure;
    const auto publish = [&](const RenderResult& result) {
      bool ready_announced = false;
      try {
        RenderResult candidate = result;
        ready.fetch_add(1U, std::memory_order_release);
        ready_announced = true;
        while (!start.load(std::memory_order_acquire)) {
          std::this_thread::yield();
        }
        if (gate.try_publish(std::move(candidate))) {
          accepted.fetch_add(1U, std::memory_order_relaxed);
        }
      } catch (...) {
        std::scoped_lock lock(failure_mutex);
        if (unexpected_failure == nullptr) {
          unexpected_failure = std::current_exception();
        }
        if (!ready_announced) {
          ready.fetch_add(1U, std::memory_order_release);
        }
      }
    };
    {
      JoiningThreadGroup publish_threads(2U);
      try {
        publish_threads.start([&] { publish(late); });
        publish_threads.start([&] { publish(current); });
      } catch (...) {
        start.store(true, std::memory_order_release);
        throw;
      }
      while (ready.load(std::memory_order_acquire) != 2U) {
        std::this_thread::yield();
      }
      start.store(true, std::memory_order_release);
    }
    if (unexpected_failure != nullptr) {
      std::rethrow_exception(unexpected_failure);
    }

    CHECK(accepted.load(std::memory_order_relaxed) == 1U);
    const auto published = gate.current();
    REQUIRE(published != nullptr);
    CHECK(published->identity == current.identity);
    CHECK(published->image == current.image);
  }
}

TEST_CASE(
    "CpuRenderer reports tile-local transient memory below whole-image "
    "multi-node intermediates") {
  const ImageF32 source = make_pattern_image(17U, 13U);
  constexpr std::size_t adjustment_count = 4U;
  const EditGraph graph = make_graph(
      {
          exposure_node("exposure-a", 1.0),
          curve_node(
              "curve-a",
              {
                  {0.0, 0.0},
                  {1.0, 1.0},
              }),
          exposure_node("exposure-b", -0.5),
          curve_node(
              "curve-b",
              {
                  {0.0, 0.1},
                  {1.0, 0.9},
              }),
      });
  const RenderRequest request = request_for(source, 4U, 1U);
  const RenderResult result =
      render_without_masks(source, graph, request);

  constexpr std::size_t expected_peak_tile_bytes =
      4U * 4U * ImageF32::channel_count * sizeof(float);
  const std::size_t one_whole_image_bytes =
      static_cast<std::size_t>(source.width) * source.height *
      ImageF32::channel_count * sizeof(float);
  const std::size_t whole_image_intermediate_bytes =
      adjustment_count * one_whole_image_bytes;

  CHECK(
      result.diagnostics.rendered_tiles ==
      tile_count(request.roi, request.tile_size));
  CHECK(
      result.diagnostics.peak_transient_tile_bytes ==
      expected_peak_tile_bytes);
  CHECK(
      result.diagnostics.peak_transient_working_set_bytes ==
      expected_peak_tile_bytes);
  CHECK(
      result.diagnostics.peak_transient_tile_bytes <
      whole_image_intermediate_bytes);
  CHECK(result.diagnostics.worker_count == 1U);
}
