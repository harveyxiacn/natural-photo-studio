#include "nps/document/edit_graph.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <ranges>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <nlohmann/json.hpp>
#include <openssl/evp.h>

namespace nps::document {
namespace {

using Json = nlohmann::json;

constexpr std::size_t kMaximumIdentifierCharacters = 128U;
constexpr std::size_t kMaximumAlgorithmVersionCharacters = 64U;
constexpr std::size_t kMaximumInputsPerNode = 16U;
constexpr std::size_t kSha256Bytes = 32U;

[[nodiscard]] bool json_nesting_is_bounded(
    std::string_view json_text,
    std::size_t maximum_depth) noexcept {
  std::size_t depth = 0U;
  bool inside_string = false;
  bool escaped = false;
  for (const char character : json_text) {
    if (inside_string) {
      if (escaped) {
        escaped = false;
      } else if (character == '\\') {
        escaped = true;
      } else if (character == '"') {
        inside_string = false;
      }
      continue;
    }
    if (character == '"') {
      inside_string = true;
    } else if (character == '{' || character == '[') {
      ++depth;
      if (depth > maximum_depth) {
        return false;
      }
    } else if (
        (character == '}' || character == ']') && depth > 0U) {
      --depth;
    }
  }
  return true;
}

[[nodiscard]] EditGraphError make_error(
    EditGraphErrorCode code,
    std::string message) {
  return EditGraphError{
      .code = code,
      .message = std::move(message),
  };
}

[[nodiscard]] bool is_ascii_alphanumeric(char value) noexcept {
  return (value >= 'a' && value <= 'z') ||
         (value >= 'A' && value <= 'Z') ||
         (value >= '0' && value <= '9');
}

[[nodiscard]] bool is_identifier_character(char value) noexcept {
  return is_ascii_alphanumeric(value) || value == '.' || value == '_' ||
         value == ':' || value == '-';
}

[[nodiscard]] bool is_stable_identifier(std::string_view value) noexcept {
  return !value.empty() && value.size() <= kMaximumIdentifierCharacters &&
         is_ascii_alphanumeric(value.front()) &&
         std::ranges::all_of(value, is_identifier_character);
}

[[nodiscard]] bool is_lower_sha256(std::string_view value) noexcept {
  return value.size() == 64U &&
         std::ranges::all_of(value, [](const char character) {
           return (character >= '0' && character <= '9') ||
                  (character >= 'a' && character <= 'f');
         });
}

[[nodiscard]] bool is_supported_algorithm_version(
    std::string_view value) noexcept {
  return value == kM1AlgorithmVersion &&
         value.size() <= kMaximumAlgorithmVersionCharacters;
}

[[nodiscard]] bool is_expected_key(
    std::string_view actual,
    std::initializer_list<std::string_view> expected) {
  return std::ranges::find(expected, actual) != expected.end();
}

[[nodiscard]] std::optional<EditGraphError> require_keys(
    const Json& value,
    std::initializer_list<std::string_view> required,
    std::initializer_list<std::string_view> optional,
    std::string_view safe_object_name) {
  if (!value.is_object()) {
    return make_error(
        EditGraphErrorCode::schema_invalid,
        std::string(safe_object_name) + " must be a JSON object.");
  }

  for (const std::string_view key : required) {
    if (!value.contains(std::string(key))) {
      return make_error(
          EditGraphErrorCode::schema_invalid,
          std::string(safe_object_name) +
              " is missing a required property.");
    }
  }

  for (const auto& [key, ignored] : value.items()) {
    static_cast<void>(ignored);
    if (!is_expected_key(key, required) &&
        !is_expected_key(key, optional)) {
      return make_error(
          EditGraphErrorCode::schema_invalid,
          std::string(safe_object_name) +
              " contains an unsupported property.");
    }
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<EditGraphError> require_string(
    const Json& object,
    std::string_view key,
    std::string_view safe_field_name) {
  if (!object.at(std::string(key)).is_string()) {
    return make_error(
        EditGraphErrorCode::schema_invalid,
        std::string(safe_field_name) + " must be a JSON string.");
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<EditGraphError> validate_identifier(
    std::string_view value,
    std::string_view safe_field_name) {
  if (!is_stable_identifier(value)) {
    return make_error(
        EditGraphErrorCode::invalid_identifier,
        std::string(safe_field_name) +
            " must use the stable ASCII identifier grammar.");
  }
  return std::nullopt;
}

[[nodiscard]] double normalized_number(double value) noexcept {
  return value == 0.0 ? 0.0 : value;
}

[[nodiscard]] std::optional<double> finite_number(const Json& value) {
  if (!value.is_number()) {
    return std::nullopt;
  }
  try {
    const double number = value.get<double>();
    if (!std::isfinite(number)) {
      return std::nullopt;
    }
    return normalized_number(number);
  } catch (const Json::exception&) {
    return std::nullopt;
  }
}

[[nodiscard]] std::optional<EditGraphError> validate_mask(
    const MaskBinding& mask) {
  if (!is_stable_identifier(mask.mask_id)) {
    return make_error(
        EditGraphErrorCode::invalid_mask,
        "mask.maskId must use the stable ASCII identifier grammar.");
  }
  if (!is_lower_sha256(mask.content_hash)) {
    return make_error(
        EditGraphErrorCode::invalid_mask,
        "mask.contentHash must be a lowercase SHA-256 value.");
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<EditGraphError> validate_curve_points(
    const std::vector<CurveControlPoint>& points) {
  if (points.size() < 2U || points.size() > kMaximumCurveControlPoints) {
    return make_error(
        EditGraphErrorCode::invalid_parameters,
        "An RGB curve must contain between 2 and 256 control points.");
  }

  double previous_x = -1.0;
  for (const CurveControlPoint& point : points) {
    if (!std::isfinite(point.x) || !std::isfinite(point.y) ||
        point.x < 0.0 || point.x > 1.0 ||
        point.y < 0.0 || point.y > 1.0) {
      return make_error(
          EditGraphErrorCode::invalid_parameters,
          "RGB curve coordinates must be finite normalized numbers.");
    }
    if (point.x <= previous_x) {
      return make_error(
          EditGraphErrorCode::invalid_parameters,
          "RGB curve x coordinates must be strictly increasing.");
    }
    previous_x = point.x;
  }

  if (points.front().x != 0.0 || points.back().x != 1.0) {
    return make_error(
        EditGraphErrorCode::invalid_parameters,
        "An RGB curve must cover the complete input interval.");
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<EditGraphError> validate_parameters(
    const EditNode& node) {
  switch (node.kind) {
    case EditNodeKind::source:
      if (!std::holds_alternative<SourceParameters>(node.parameters)) {
        return make_error(
            EditGraphErrorCode::invalid_parameters,
            "A source node requires empty source parameters.");
      }
      return std::nullopt;
    case EditNodeKind::adjust_exposure: {
      const auto* parameters =
          std::get_if<ExposureParameters>(&node.parameters);
      if (parameters == nullptr || !std::isfinite(parameters->ev) ||
          parameters->ev < kMinimumExposureEv ||
          parameters->ev > kMaximumExposureEv) {
        return make_error(
            EditGraphErrorCode::invalid_parameters,
            "Exposure EV must be a finite number from -10 through 10.");
      }
      return std::nullopt;
    }
    case EditNodeKind::adjust_curve_rgb: {
      const auto* parameters =
          std::get_if<RgbCurveParameters>(&node.parameters);
      if (parameters == nullptr) {
        return make_error(
            EditGraphErrorCode::invalid_parameters,
            "An RGB curve node requires curve parameters.");
      }
      return validate_curve_points(parameters->points);
    }
    case EditNodeKind::output:
      if (!std::holds_alternative<OutputParameters>(node.parameters)) {
        return make_error(
            EditGraphErrorCode::invalid_parameters,
            "An output node requires empty output parameters.");
      }
      return std::nullopt;
  }

  return make_error(
      EditGraphErrorCode::unknown_critical_node,
      "The graph contains an unknown critical node type.");
}

[[nodiscard]] std::optional<EditGraphError> validate_node_shape(
    const EditNode& node) {
  if (auto error = validate_identifier(node.node_id, "nodeId")) {
    return error;
  }
  if (!is_supported_algorithm_version(node.algorithm_version)) {
    return make_error(
        EditGraphErrorCode::unsupported_algorithm_version,
        "A node uses an unsupported algorithm version.");
  }
  if (node.compute_domain != ComputeDomain::scene_linear) {
    return make_error(
        EditGraphErrorCode::invalid_compute_domain,
        "M1 nodes must use the scene-linear compute domain.");
  }
  if (!std::isfinite(node.opacity) ||
      node.opacity < 0.0 || node.opacity > 1.0) {
    return make_error(
        EditGraphErrorCode::invalid_opacity,
        "Node opacity must be a finite number from 0 through 1.");
  }
  if (node.inputs.size() > kMaximumInputsPerNode) {
    return make_error(
        EditGraphErrorCode::invalid_topology,
        "A node declares too many inputs.");
  }

  std::unordered_set<std::string_view> input_ids;
  input_ids.reserve(node.inputs.size());
  for (const std::string& input : node.inputs) {
    if (!is_stable_identifier(input)) {
      return make_error(
          EditGraphErrorCode::invalid_identifier,
          "An input node reference uses an invalid identifier.");
    }
    if (!input_ids.insert(input).second) {
      return make_error(
          EditGraphErrorCode::invalid_topology,
          "A node declares the same input more than once.");
    }
  }

  if (node.mask.has_value()) {
    if (node.kind == EditNodeKind::source ||
        node.kind == EditNodeKind::output) {
      return make_error(
          EditGraphErrorCode::invalid_mask,
          "Source and output nodes cannot bind a mask.");
    }
    if (auto error = validate_mask(*node.mask)) {
      return error;
    }
  }

  switch (node.kind) {
    case EditNodeKind::source:
      if (!node.enabled || node.opacity != 1.0 || !node.inputs.empty()) {
        return make_error(
            EditGraphErrorCode::invalid_source,
            "A source node must be enabled, fully opaque, and have no inputs.");
      }
      break;
    case EditNodeKind::adjust_exposure:
    case EditNodeKind::adjust_curve_rgb:
      if (node.inputs.size() != 1U) {
        return make_error(
            EditGraphErrorCode::invalid_topology,
            "M1 adjustment nodes require exactly one input.");
      }
      break;
    case EditNodeKind::output:
      if (!node.enabled || node.opacity != 1.0 ||
          node.inputs.size() != 1U) {
        return make_error(
            EditGraphErrorCode::invalid_output,
            "An output node must be enabled, fully opaque, and have one input.");
      }
      break;
    default:
      return make_error(
          EditGraphErrorCode::unknown_critical_node,
          "The graph contains an unknown critical node type.");
  }

  return validate_parameters(node);
}

[[nodiscard]] std::optional<EditGraphError> validate_topology(
    const EditGraphDefinition& definition) {
  std::unordered_map<std::string_view, const EditNode*> by_id;
  by_id.reserve(definition.nodes.size());

  std::size_t source_count = 0U;
  std::size_t output_count = 0U;
  for (const EditNode& node : definition.nodes) {
    if (!by_id.emplace(node.node_id, &node).second) {
      return make_error(
          EditGraphErrorCode::duplicate_node_id,
          "Every nodeId in an edit graph must be unique.");
    }
    if (node.kind == EditNodeKind::source) {
      ++source_count;
    } else if (node.kind == EditNodeKind::output) {
      ++output_count;
    }
  }

  if (source_count != 1U) {
    return make_error(
        EditGraphErrorCode::invalid_source,
        "An edit graph must contain exactly one source node.");
  }
  if (output_count != 1U) {
    return make_error(
        EditGraphErrorCode::invalid_output,
        "An edit graph must contain exactly one output node.");
  }

  const auto source = by_id.find(definition.source_node_id);
  if (source == by_id.end() ||
      source->second->kind != EditNodeKind::source) {
    return make_error(
        EditGraphErrorCode::invalid_source,
        "sourceNodeId must identify the graph's source node.");
  }
  const auto output = by_id.find(definition.output_node_id);
  if (output == by_id.end() ||
      output->second->kind != EditNodeKind::output) {
    return make_error(
        EditGraphErrorCode::invalid_output,
        "outputNodeId must identify the graph's output node.");
  }

  for (const EditNode& node : definition.nodes) {
    for (const std::string& input : node.inputs) {
      if (!by_id.contains(input)) {
        return make_error(
            EditGraphErrorCode::missing_input,
            "A node refers to an input that is not present in the graph.");
      }
    }
  }

  enum class VisitState : std::uint8_t {
    unseen,
    visiting,
    complete,
  };
  std::unordered_map<std::string_view, VisitState> state;
  state.reserve(definition.nodes.size());
  for (const EditNode& node : definition.nodes) {
    state.emplace(node.node_id, VisitState::unseen);
  }

  const auto visit = [&](const auto& self, const EditNode& node)
      -> std::optional<EditGraphError> {
    VisitState& current = state.at(node.node_id);
    if (current == VisitState::visiting) {
      return make_error(
          EditGraphErrorCode::cycle_detected,
          "The edit graph must be acyclic.");
    }
    if (current == VisitState::complete) {
      return std::nullopt;
    }
    current = VisitState::visiting;
    for (const std::string& input : node.inputs) {
      if (auto error = self(self, *by_id.at(input))) {
        return error;
      }
    }
    current = VisitState::complete;
    return std::nullopt;
  };

  for (const EditNode& node : definition.nodes) {
    if (auto error = visit(visit, node)) {
      return error;
    }
  }

  std::unordered_set<std::string_view> output_ancestors;
  output_ancestors.reserve(definition.nodes.size());
  const auto collect_ancestors = [&](const auto& self, const EditNode& node)
      -> void {
    if (!output_ancestors.insert(node.node_id).second) {
      return;
    }
    for (const std::string& input : node.inputs) {
      self(self, *by_id.at(input));
    }
  };
  collect_ancestors(collect_ancestors, *output->second);

  if (output_ancestors.size() != definition.nodes.size() ||
      !output_ancestors.contains(definition.source_node_id)) {
    return make_error(
        EditGraphErrorCode::invalid_topology,
        "Every node must lie on the declared source-to-output path.");
  }

  return std::nullopt;
}

[[nodiscard]] std::variant<MaskBinding, EditGraphError> parse_mask(
    const Json& value) {
  if (auto error =
          require_keys(
              value,
              {"maskId", "contentHash", "inverted"},
              {},
              "mask")) {
    return *std::move(error);
  }
  if (auto error = require_string(value, "maskId", "mask.maskId")) {
    return *std::move(error);
  }
  if (auto error =
          require_string(value, "contentHash", "mask.contentHash")) {
    return *std::move(error);
  }
  if (!value.at("inverted").is_boolean()) {
    return make_error(
        EditGraphErrorCode::invalid_mask,
        "mask.inverted must be a JSON boolean.");
  }

  MaskBinding mask{
      .mask_id = value.at("maskId").get_ref<const std::string&>(),
      .content_hash =
          value.at("contentHash").get_ref<const std::string&>(),
      .inverted = value.at("inverted").get<bool>(),
  };
  if (auto error = validate_mask(mask)) {
    return *std::move(error);
  }
  return mask;
}

[[nodiscard]] std::variant<EditNodeParameters, EditGraphError>
parse_parameters(EditNodeKind kind, const Json& value) {
  switch (kind) {
    case EditNodeKind::source:
    case EditNodeKind::output: {
      if (auto error = require_keys(value, {}, {}, "parameters")) {
        return *std::move(error);
      }
      if (kind == EditNodeKind::source) {
        return EditNodeParameters{SourceParameters{}};
      }
      return EditNodeParameters{OutputParameters{}};
    }
    case EditNodeKind::adjust_exposure: {
      if (auto error = require_keys(value, {"ev"}, {}, "parameters")) {
        return *std::move(error);
      }
      const std::optional<double> ev = finite_number(value.at("ev"));
      if (!ev.has_value() || *ev < kMinimumExposureEv ||
          *ev > kMaximumExposureEv) {
        return make_error(
            EditGraphErrorCode::invalid_parameters,
            "Exposure EV must be a finite number from -10 through 10.");
      }
      return EditNodeParameters{ExposureParameters{.ev = *ev}};
    }
    case EditNodeKind::adjust_curve_rgb: {
      if (auto error = require_keys(value, {"points"}, {}, "parameters")) {
        return *std::move(error);
      }
      const Json& points_json = value.at("points");
      if (!points_json.is_array() || points_json.size() < 2U ||
          points_json.size() > kMaximumCurveControlPoints) {
        return make_error(
            EditGraphErrorCode::invalid_parameters,
            "An RGB curve must contain between 2 and 256 control points.");
      }

      std::vector<CurveControlPoint> points;
      points.reserve(points_json.size());
      for (const Json& point_json : points_json) {
        if (auto error =
                require_keys(point_json, {"x", "y"}, {}, "curve point")) {
          return *std::move(error);
        }
        const std::optional<double> x = finite_number(point_json.at("x"));
        const std::optional<double> y = finite_number(point_json.at("y"));
        if (!x.has_value() || !y.has_value()) {
          return make_error(
              EditGraphErrorCode::invalid_parameters,
              "RGB curve coordinates must be finite JSON numbers.");
        }
        points.push_back(CurveControlPoint{.x = *x, .y = *y});
      }
      if (auto error = validate_curve_points(points)) {
        return *std::move(error);
      }
      return EditNodeParameters{
          RgbCurveParameters{.points = std::move(points)}};
    }
  }

  return make_error(
      EditGraphErrorCode::unknown_critical_node,
      "The graph contains an unknown critical node type.");
}

[[nodiscard]] std::variant<EditNode, EditGraphError> parse_node(
    const Json& value) {
  if (auto error = require_keys(
          value,
          {
              "nodeId",
              "type",
              "algorithmVersion",
              "enabled",
              "opacity",
              "computeDomain",
              "inputs",
              "parameters",
          },
          {"mask"},
          "node")) {
    return *std::move(error);
  }

  for (const std::string_view field :
       {"nodeId", "type", "algorithmVersion", "computeDomain"}) {
    if (auto error = require_string(value, field, field)) {
      return *std::move(error);
    }
  }

  const auto kind = edit_node_kind_from_string(
      value.at("type").get_ref<const std::string&>());
  if (!kind.has_value()) {
    return make_error(
        EditGraphErrorCode::unknown_critical_node,
        "The graph contains an unknown critical node type.");
  }
  const auto domain = compute_domain_from_string(
      value.at("computeDomain").get_ref<const std::string&>());
  if (!domain.has_value()) {
    return make_error(
        EditGraphErrorCode::invalid_compute_domain,
        "A node declares an unsupported compute domain.");
  }
  if (!value.at("enabled").is_boolean()) {
    return make_error(
        EditGraphErrorCode::schema_invalid,
        "enabled must be a JSON boolean.");
  }
  const std::optional<double> opacity = finite_number(value.at("opacity"));
  if (!opacity.has_value() || *opacity < 0.0 || *opacity > 1.0) {
    return make_error(
        EditGraphErrorCode::invalid_opacity,
        "Node opacity must be a finite number from 0 through 1.");
  }

  const Json& inputs_json = value.at("inputs");
  if (!inputs_json.is_array() ||
      inputs_json.size() > kMaximumInputsPerNode) {
    return make_error(
        EditGraphErrorCode::invalid_topology,
        "inputs must be a bounded JSON array.");
  }
  std::vector<std::string> inputs;
  inputs.reserve(inputs_json.size());
  for (const Json& input_json : inputs_json) {
    if (!input_json.is_string()) {
      return make_error(
          EditGraphErrorCode::schema_invalid,
          "Every input reference must be a JSON string.");
    }
    inputs.push_back(input_json.get_ref<const std::string&>());
  }

  auto parsed_parameters = parse_parameters(*kind, value.at("parameters"));
  if (std::holds_alternative<EditGraphError>(parsed_parameters)) {
    return std::get<EditGraphError>(std::move(parsed_parameters));
  }

  std::optional<MaskBinding> mask;
  if (value.contains("mask")) {
    auto parsed_mask = parse_mask(value.at("mask"));
    if (std::holds_alternative<EditGraphError>(parsed_mask)) {
      return std::get<EditGraphError>(std::move(parsed_mask));
    }
    mask = std::get<MaskBinding>(std::move(parsed_mask));
  }

  EditNode node{
      .node_id = value.at("nodeId").get_ref<const std::string&>(),
      .kind = *kind,
      .algorithm_version =
          value.at("algorithmVersion").get_ref<const std::string&>(),
      .enabled = value.at("enabled").get<bool>(),
      .opacity = *opacity,
      .compute_domain = *domain,
      .inputs = std::move(inputs),
      .parameters =
          std::get<EditNodeParameters>(std::move(parsed_parameters)),
      .mask = std::move(mask),
  };
  if (auto error = validate_node_shape(node)) {
    return *std::move(error);
  }
  return node;
}

[[nodiscard]] Json mask_to_json(const MaskBinding& mask) {
  return Json{
      {"maskId", mask.mask_id},
      {"contentHash", mask.content_hash},
      {"inverted", mask.inverted},
  };
}

[[nodiscard]] Json parameters_to_json(const EditNode& node) {
  switch (node.kind) {
    case EditNodeKind::source:
      return Json::object();
    case EditNodeKind::adjust_exposure:
      return Json{{
          "ev",
          normalized_number(std::get<ExposureParameters>(node.parameters).ev),
      }};
    case EditNodeKind::adjust_curve_rgb: {
      Json points = Json::array();
      for (const CurveControlPoint& point :
           std::get<RgbCurveParameters>(node.parameters).points) {
        points.push_back(Json{
            {"x", normalized_number(point.x)},
            {"y", normalized_number(point.y)},
        });
      }
      return Json{{"points", std::move(points)}};
    }
    case EditNodeKind::output:
      return Json::object();
  }
  throw std::logic_error("The edit graph contains an invalid node kind.");
}

[[nodiscard]] Json node_to_json(const EditNode& node) {
  Json value{
      {"nodeId", node.node_id},
      {"type", std::string(to_string(node.kind))},
      {"algorithmVersion", node.algorithm_version},
      {"enabled", node.enabled},
      {"opacity", normalized_number(node.opacity)},
      {"computeDomain", std::string(to_string(node.compute_domain))},
      {"inputs", node.inputs},
      {"parameters", parameters_to_json(node)},
  };
  if (node.mask.has_value()) {
    value["mask"] = mask_to_json(*node.mask);
  }
  return value;
}

}  // namespace

std::string_view to_string(EditNodeKind kind) noexcept {
  switch (kind) {
    case EditNodeKind::source:
      return "source";
    case EditNodeKind::adjust_exposure:
      return "adjust.exposure";
    case EditNodeKind::adjust_curve_rgb:
      return "adjust.curve.rgb";
    case EditNodeKind::output:
      return "output";
  }
  return "<invalid-edit-node-kind>";
}

std::optional<EditNodeKind> edit_node_kind_from_string(
    std::string_view value) noexcept {
  if (value == "source") {
    return EditNodeKind::source;
  }
  if (value == "adjust.exposure") {
    return EditNodeKind::adjust_exposure;
  }
  if (value == "adjust.curve.rgb") {
    return EditNodeKind::adjust_curve_rgb;
  }
  if (value == "output") {
    return EditNodeKind::output;
  }
  return std::nullopt;
}

std::string_view to_string(ComputeDomain domain) noexcept {
  switch (domain) {
    case ComputeDomain::scene_linear:
      return "scene-linear";
  }
  return "<invalid-compute-domain>";
}

std::optional<ComputeDomain> compute_domain_from_string(
    std::string_view value) noexcept {
  if (value == "scene-linear") {
    return ComputeDomain::scene_linear;
  }
  return std::nullopt;
}

std::string_view to_string(EditGraphErrorCode code) noexcept {
  switch (code) {
    case EditGraphErrorCode::invalid_json:
      return "EDIT_GRAPH_JSON_INVALID";
    case EditGraphErrorCode::schema_invalid:
      return "EDIT_GRAPH_SCHEMA_INVALID";
    case EditGraphErrorCode::invalid_identifier:
      return "EDIT_GRAPH_IDENTIFIER_INVALID";
    case EditGraphErrorCode::unknown_critical_node:
      return "EDIT_GRAPH_CRITICAL_NODE_UNKNOWN";
    case EditGraphErrorCode::unsupported_algorithm_version:
      return "EDIT_GRAPH_ALGORITHM_UNSUPPORTED";
    case EditGraphErrorCode::duplicate_node_id:
      return "EDIT_GRAPH_NODE_ID_DUPLICATE";
    case EditGraphErrorCode::invalid_source:
      return "EDIT_GRAPH_SOURCE_INVALID";
    case EditGraphErrorCode::invalid_output:
      return "EDIT_GRAPH_OUTPUT_INVALID";
    case EditGraphErrorCode::invalid_compute_domain:
      return "EDIT_GRAPH_COMPUTE_DOMAIN_INVALID";
    case EditGraphErrorCode::invalid_opacity:
      return "EDIT_GRAPH_OPACITY_INVALID";
    case EditGraphErrorCode::invalid_mask:
      return "EDIT_GRAPH_MASK_INVALID";
    case EditGraphErrorCode::invalid_parameters:
      return "EDIT_GRAPH_PARAMETERS_INVALID";
    case EditGraphErrorCode::missing_input:
      return "EDIT_GRAPH_INPUT_MISSING";
    case EditGraphErrorCode::cycle_detected:
      return "EDIT_GRAPH_CYCLE";
    case EditGraphErrorCode::invalid_topology:
      return "EDIT_GRAPH_TOPOLOGY_INVALID";
  }
  return "EDIT_GRAPH_INTERNAL_UNKNOWN_ERROR";
}

EditGraph::EditGraph(EditGraphDefinition definition)
    : definition_(std::move(definition)) {}

const std::string& EditGraph::graph_id() const noexcept {
  return definition_.graph_id;
}

const std::string& EditGraph::working_color_space() const noexcept {
  return definition_.working_color_space;
}

const std::string& EditGraph::source_node_id() const noexcept {
  return definition_.source_node_id;
}

const std::string& EditGraph::output_node_id() const noexcept {
  return definition_.output_node_id;
}

std::span<const EditNode> EditGraph::nodes() const noexcept {
  return definition_.nodes;
}

const EditNode* EditGraph::find_node(std::string_view node_id) const noexcept {
  const auto found = std::ranges::lower_bound(
      definition_.nodes,
      node_id,
      {},
      &EditNode::node_id);
  if (found == definition_.nodes.end() || found->node_id != node_id) {
    return nullptr;
  }
  return &*found;
}

std::vector<const EditNode*> EditGraph::nodes_in_topological_order() const {
  std::vector<const EditNode*> result;
  result.reserve(definition_.nodes.size());
  std::unordered_set<std::string_view> visited;
  visited.reserve(definition_.nodes.size());

  const auto append_dependencies =
      [&](const auto& self, const EditNode& node) -> void {
    if (!visited.insert(node.node_id).second) {
      return;
    }
    for (const std::string& input : node.inputs) {
      self(self, *find_node(input));
    }
    result.push_back(&node);
  };
  append_dependencies(
      append_dependencies,
      *find_node(definition_.output_node_id));
  return result;
}

EditGraphResult create_edit_graph(EditGraphDefinition definition) {
  if (auto error = validate_identifier(definition.graph_id, "graphId")) {
    return *std::move(error);
  }
  if (definition.working_color_space !=
      kSceneLinearRec2020D65WorkingColorSpace) {
    return make_error(
        EditGraphErrorCode::schema_invalid,
        "workingColorSpace must use the M1 scene-linear Rec.2020 D65 ID.");
  }
  if (auto error =
          validate_identifier(definition.source_node_id, "sourceNodeId")) {
    return *std::move(error);
  }
  if (auto error =
          validate_identifier(definition.output_node_id, "outputNodeId")) {
    return *std::move(error);
  }
  if (definition.nodes.empty() ||
      definition.nodes.size() > kMaximumEditGraphNodes) {
    return make_error(
        EditGraphErrorCode::invalid_topology,
        "An edit graph must contain between 1 and 4096 nodes.");
  }

  for (EditNode& node : definition.nodes) {
    node.opacity = normalized_number(node.opacity);
    if (auto* exposure =
            std::get_if<ExposureParameters>(&node.parameters)) {
      exposure->ev = normalized_number(exposure->ev);
    }
    if (auto* curve =
            std::get_if<RgbCurveParameters>(&node.parameters)) {
      for (CurveControlPoint& point : curve->points) {
        point.x = normalized_number(point.x);
        point.y = normalized_number(point.y);
      }
    }
    if (auto error = validate_node_shape(node)) {
      return *std::move(error);
    }
  }

  if (auto error = validate_topology(definition)) {
    return *std::move(error);
  }

  std::ranges::sort(definition.nodes, {}, &EditNode::node_id);
  return EditGraph(std::move(definition));
}

EditGraphBuilder::EditGraphBuilder(std::string graph_id)
    : definition_{
          .graph_id = std::move(graph_id),
          .working_color_space =
              std::string(kSceneLinearRec2020D65WorkingColorSpace),
          .source_node_id = {},
          .output_node_id = {},
          .nodes = {},
      } {}

EditGraphBuilder::EditGraphBuilder(EditGraphDefinition definition)
    : definition_(std::move(definition)) {}

EditGraphBuilder EditGraphBuilder::from_graph(const EditGraph& graph) {
  return EditGraphBuilder(graph.definition_);
}

EditGraphBuilder& EditGraphBuilder::set_working_color_space(
    std::string value) {
  definition_.working_color_space = std::move(value);
  return *this;
}

EditGraphBuilder& EditGraphBuilder::set_source_node_id(std::string value) {
  definition_.source_node_id = std::move(value);
  return *this;
}

EditGraphBuilder& EditGraphBuilder::set_output_node_id(std::string value) {
  definition_.output_node_id = std::move(value);
  return *this;
}

bool EditGraphBuilder::add_node(EditNode node) {
  if (std::ranges::any_of(
          definition_.nodes,
          [&node](const EditNode& existing) {
            return existing.node_id == node.node_id;
          })) {
    return false;
  }
  definition_.nodes.push_back(std::move(node));
  return true;
}

bool EditGraphBuilder::replace_node(
    std::string_view existing_node_id,
    EditNode replacement) {
  if (replacement.node_id != existing_node_id) {
    return false;
  }
  const auto found = std::ranges::find(
      definition_.nodes,
      existing_node_id,
      &EditNode::node_id);
  if (found == definition_.nodes.end()) {
    return false;
  }
  *found = std::move(replacement);
  return true;
}

bool EditGraphBuilder::remove_node(std::string_view node_id) {
  const auto found = std::ranges::find(
      definition_.nodes,
      node_id,
      &EditNode::node_id);
  if (found == definition_.nodes.end()) {
    return false;
  }
  definition_.nodes.erase(found);
  return true;
}

EditGraphResult EditGraphBuilder::build() const {
  return create_edit_graph(definition_);
}

EditGraphResult parse_edit_graph_json(std::string_view json_text) {
  if (json_text.empty() ||
      json_text.size() > kMaximumEditGraphJsonBytes ||
      !json_nesting_is_bounded(
          json_text, kMaximumEditGraphJsonNestingDepth)) {
    return make_error(
        EditGraphErrorCode::invalid_json,
        "The edit graph is not valid bounded JSON.");
  }

  bool duplicate_property = false;
  std::vector<std::unordered_set<std::string>> object_keys;
  const Json::parser_callback_t callback =
      [&duplicate_property, &object_keys](
          int,
          Json::parse_event_t event,
          Json& parsed) {
        if (event == Json::parse_event_t::object_start) {
          object_keys.emplace_back();
        } else if (event == Json::parse_event_t::key) {
          if (object_keys.empty() ||
              !object_keys.back()
                   .insert(parsed.get_ref<const std::string&>())
                   .second) {
            duplicate_property = true;
          }
        } else if (event == Json::parse_event_t::object_end &&
                   !object_keys.empty()) {
          object_keys.pop_back();
        }
        return true;
      };

  Json root;
  try {
    root = Json::parse(
        json_text.begin(),
        json_text.end(),
        callback,
        true,
        false);
  } catch (const Json::exception&) {
    return make_error(
        EditGraphErrorCode::invalid_json,
        "The edit graph is not valid JSON.");
  }
  if (duplicate_property) {
    return make_error(
        EditGraphErrorCode::schema_invalid,
        "Duplicate JSON object properties are not allowed.");
  }

  if (auto error = require_keys(
          root,
          {
              "schema",
              "graphId",
              "workingColorSpace",
              "sourceNodeId",
              "outputNodeId",
              "nodes",
          },
          {},
          "edit graph envelope")) {
    return *std::move(error);
  }
  for (const std::string_view field : {
           "schema",
           "graphId",
           "workingColorSpace",
           "sourceNodeId",
           "outputNodeId",
       }) {
    if (auto error = require_string(root, field, field)) {
      return *std::move(error);
    }
  }
  if (root.at("schema").get_ref<const std::string&>() !=
      kEditGraphSchema) {
    return make_error(
        EditGraphErrorCode::schema_invalid,
        "schema must be exactly 'nps.edit-graph/v1'.");
  }
  if (!root.at("nodes").is_array() ||
      root.at("nodes").empty() ||
      root.at("nodes").size() > kMaximumEditGraphNodes) {
    return make_error(
        EditGraphErrorCode::invalid_topology,
        "nodes must be a non-empty bounded JSON array.");
  }

  std::vector<EditNode> nodes;
  nodes.reserve(root.at("nodes").size());
  for (const Json& node_json : root.at("nodes")) {
    auto parsed_node = parse_node(node_json);
    if (std::holds_alternative<EditGraphError>(parsed_node)) {
      return std::get<EditGraphError>(std::move(parsed_node));
    }
    nodes.push_back(std::get<EditNode>(std::move(parsed_node)));
  }

  return create_edit_graph(EditGraphDefinition{
      .graph_id = root.at("graphId").get_ref<const std::string&>(),
      .working_color_space =
          root.at("workingColorSpace").get_ref<const std::string&>(),
      .source_node_id =
          root.at("sourceNodeId").get_ref<const std::string&>(),
      .output_node_id =
          root.at("outputNodeId").get_ref<const std::string&>(),
      .nodes = std::move(nodes),
  });
}

nlohmann::json edit_graph_to_json(const EditGraph& graph) {
  Json nodes = Json::array();
  for (const EditNode& node : graph.nodes()) {
    nodes.push_back(node_to_json(node));
  }
  return Json{
      {"schema", std::string(kEditGraphSchema)},
      {"graphId", graph.graph_id()},
      {"workingColorSpace", graph.working_color_space()},
      {"sourceNodeId", graph.source_node_id()},
      {"outputNodeId", graph.output_node_id()},
      {"nodes", std::move(nodes)},
  };
}

std::string canonical_edit_graph_json(const EditGraph& graph) {
  return edit_graph_to_json(graph).dump();
}

std::string edit_graph_sha256(const EditGraph& graph) {
  const std::string canonical = canonical_edit_graph_json(graph);
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned int digest_size = 0U;
  EVP_MD_CTX* context = EVP_MD_CTX_new();
  if (context == nullptr) {
    throw std::runtime_error("Unable to allocate the edit graph hash context.");
  }

  const auto release_context = [&context]() noexcept {
    EVP_MD_CTX_free(context);
    context = nullptr;
  };
  if (EVP_DigestInit_ex(context, EVP_sha256(), nullptr) != 1 ||
      EVP_DigestUpdate(context, canonical.data(), canonical.size()) != 1 ||
      EVP_DigestFinal_ex(context, digest.data(), &digest_size) != 1) {
    release_context();
    throw std::runtime_error("Unable to calculate the edit graph hash.");
  }
  release_context();
  if (digest_size != kSha256Bytes) {
    throw std::runtime_error(
        "The SHA-256 provider returned an invalid digest size.");
  }

  constexpr std::array<char, 16> hexadecimal{
      '0', '1', '2', '3', '4', '5', '6', '7',
      '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
  std::string result;
  result.reserve(kSha256Bytes * 2U);
  for (std::size_t index = 0; index < kSha256Bytes; ++index) {
    const unsigned char value = digest[index];
    result.push_back(
        hexadecimal[static_cast<std::size_t>(value >> 4U)]);
    result.push_back(
        hexadecimal[static_cast<std::size_t>(value & 0x0FU)]);
  }
  return result;
}

}  // namespace nps::document
