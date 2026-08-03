#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "nps/color/color_encoding.hpp"

namespace nps::document {

inline constexpr std::string_view kEditGraphSchema = "nps.edit-graph/v1";
inline constexpr std::string_view kSceneLinearRec2020D65WorkingColorSpace =
    nps::color::scene_linear_rec2020_d65_id;
inline constexpr std::string_view kM1AlgorithmVersion = "1.0.0";
inline constexpr double kMinimumExposureEv = -10.0;
inline constexpr double kMaximumExposureEv = 10.0;
inline constexpr std::size_t kMaximumEditGraphJsonBytes =
    4U * 1024U * 1024U;
inline constexpr std::size_t kMaximumEditGraphJsonNestingDepth = 64U;
inline constexpr std::size_t kMaximumEditGraphNodes = 4096U;
inline constexpr std::size_t kMaximumCurveControlPoints = 256U;

enum class EditNodeKind {
  source,
  adjust_exposure,
  adjust_curve_rgb,
  output,
};

[[nodiscard]] std::string_view to_string(EditNodeKind kind) noexcept;
[[nodiscard]] std::optional<EditNodeKind> edit_node_kind_from_string(
    std::string_view value) noexcept;

enum class ComputeDomain {
  scene_linear,
};

[[nodiscard]] std::string_view to_string(ComputeDomain domain) noexcept;
[[nodiscard]] std::optional<ComputeDomain> compute_domain_from_string(
    std::string_view value) noexcept;

struct MaskBinding {
  std::string mask_id;
  std::string content_hash;
  bool inverted{};

  [[nodiscard]] bool operator==(const MaskBinding&) const = default;
};

struct CurveControlPoint {
  double x{};
  double y{};

  [[nodiscard]] bool operator==(const CurveControlPoint&) const = default;
};

struct SourceParameters {
  [[nodiscard]] bool operator==(const SourceParameters&) const = default;
};

struct ExposureParameters {
  double ev{};

  [[nodiscard]] bool operator==(const ExposureParameters&) const = default;
};

struct RgbCurveParameters {
  std::vector<CurveControlPoint> points;

  [[nodiscard]] bool operator==(const RgbCurveParameters&) const = default;
};

struct OutputParameters {
  [[nodiscard]] bool operator==(const OutputParameters&) const = default;
};

using EditNodeParameters = std::variant<
    SourceParameters,
    ExposureParameters,
    RgbCurveParameters,
    OutputParameters>;

// An EditNode is a construction value. Once accepted by create_edit_graph, the
// graph owns its copy and exposes nodes only through const access.
struct EditNode {
  std::string node_id;
  EditNodeKind kind{EditNodeKind::source};
  std::string algorithm_version{std::string(kM1AlgorithmVersion)};
  bool enabled{true};
  double opacity{1.0};
  ComputeDomain compute_domain{ComputeDomain::scene_linear};
  std::vector<std::string> inputs;
  EditNodeParameters parameters{SourceParameters{}};
  std::optional<MaskBinding> mask;

  [[nodiscard]] bool operator==(const EditNode&) const = default;
};

struct EditGraphDefinition {
  std::string graph_id;
  std::string working_color_space{
      std::string(kSceneLinearRec2020D65WorkingColorSpace)};
  std::string source_node_id;
  std::string output_node_id;
  std::vector<EditNode> nodes;

  [[nodiscard]] bool operator==(const EditGraphDefinition&) const = default;
};

enum class EditGraphErrorCode {
  invalid_json,
  schema_invalid,
  invalid_identifier,
  unknown_critical_node,
  unsupported_algorithm_version,
  duplicate_node_id,
  invalid_source,
  invalid_output,
  invalid_compute_domain,
  invalid_opacity,
  invalid_mask,
  invalid_parameters,
  missing_input,
  cycle_detected,
  invalid_topology,
};

[[nodiscard]] std::string_view to_string(EditGraphErrorCode code) noexcept;

struct EditGraphError {
  EditGraphErrorCode code{EditGraphErrorCode::schema_invalid};
  std::string message;

  [[nodiscard]] bool operator==(const EditGraphError&) const = default;
};

class EditGraph final {
 public:
  EditGraph(const EditGraph&) = default;
  EditGraph(EditGraph&&) noexcept = default;
  EditGraph& operator=(const EditGraph&) = default;
  EditGraph& operator=(EditGraph&&) noexcept = default;
  ~EditGraph() = default;

  [[nodiscard]] const std::string& graph_id() const noexcept;
  [[nodiscard]] const std::string& working_color_space() const noexcept;
  [[nodiscard]] const std::string& source_node_id() const noexcept;
  [[nodiscard]] const std::string& output_node_id() const noexcept;
  [[nodiscard]] std::span<const EditNode> nodes() const noexcept;
  [[nodiscard]] const EditNode* find_node(
      std::string_view node_id) const noexcept;
  // Returns source-first dependency order. Pointers remain valid for the
  // lifetime of this immutable graph.
  [[nodiscard]] std::vector<const EditNode*> nodes_in_topological_order()
      const;

  [[nodiscard]] bool operator==(const EditGraph&) const = default;

 private:
  explicit EditGraph(EditGraphDefinition definition);

  EditGraphDefinition definition_;

  friend std::variant<EditGraph, EditGraphError> create_edit_graph(
      EditGraphDefinition definition);
  friend class EditGraphBuilder;
};

using EditGraphResult = std::variant<EditGraph, EditGraphError>;

// Validates and normalizes a graph definition. Node storage is sorted by
// nodeId, so semantically equivalent node-array orderings serialize equally.
[[nodiscard]] EditGraphResult create_edit_graph(
    EditGraphDefinition definition);

// Mutable draft helper for composing an atomic graph change. It never exposes
// or mutates EditGraph storage; build() always performs full validation and
// returns a new immutable graph.
class EditGraphBuilder final {
 public:
  explicit EditGraphBuilder(std::string graph_id);

  [[nodiscard]] static EditGraphBuilder from_graph(const EditGraph& graph);

  EditGraphBuilder& set_working_color_space(std::string value);
  EditGraphBuilder& set_source_node_id(std::string value);
  EditGraphBuilder& set_output_node_id(std::string value);

  // Returns false when add would duplicate an existing draft ID, replace or
  // remove cannot find the requested ID, or replace changes the stable ID.
  [[nodiscard]] bool add_node(EditNode node);
  [[nodiscard]] bool replace_node(
      std::string_view existing_node_id,
      EditNode replacement);
  [[nodiscard]] bool remove_node(std::string_view node_id);

  [[nodiscard]] EditGraphResult build() const;

 private:
  explicit EditGraphBuilder(EditGraphDefinition definition);

  EditGraphDefinition definition_;
};

// Parses the strict nps.edit-graph/v1 envelope. Unknown and duplicate object
// properties are rejected. Errors never echo JSON input or filesystem paths.
[[nodiscard]] EditGraphResult parse_edit_graph_json(
    std::string_view json_text);

[[nodiscard]] nlohmann::json edit_graph_to_json(const EditGraph& graph);

// Returns normalized compact UTF-8 JSON suitable for content addressing.
[[nodiscard]] std::string canonical_edit_graph_json(const EditGraph& graph);

// Returns lowercase hexadecimal SHA-256 over canonical_edit_graph_json().
// Throws std::runtime_error only if the cryptographic provider fails.
[[nodiscard]] std::string edit_graph_sha256(const EditGraph& graph);

}  // namespace nps::document
