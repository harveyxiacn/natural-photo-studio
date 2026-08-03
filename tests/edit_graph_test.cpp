#include "nps/document/edit_graph.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

namespace {

using Json = nlohmann::json;
using nps::document::ComputeDomain;
using nps::document::CurveControlPoint;
using nps::document::EditGraph;
using nps::document::EditGraphBuilder;
using nps::document::EditGraphDefinition;
using nps::document::EditGraphError;
using nps::document::EditGraphErrorCode;
using nps::document::EditGraphResult;
using nps::document::EditNode;
using nps::document::EditNodeKind;
using nps::document::ExposureParameters;
using nps::document::MaskBinding;
using nps::document::OutputParameters;
using nps::document::RgbCurveParameters;
using nps::document::SourceParameters;

[[nodiscard]] std::string mask_content_hash() {
  return std::string(64U, 'c');
}

[[nodiscard]] Json node_json(
    std::string node_id,
    std::string type,
    Json inputs,
    Json parameters) {
  return Json{
      {"nodeId", std::move(node_id)},
      {"type", std::move(type)},
      {"algorithmVersion", "1.0.0"},
      {"enabled", true},
      {"opacity", 1.0},
      {"computeDomain", "scene-linear"},
      {"inputs", std::move(inputs)},
      {"parameters", std::move(parameters)},
  };
}

[[nodiscard]] Json valid_graph_json() {
  Json source = node_json(
      "node-source", "source", Json::array(), Json::object());
  Json exposure = node_json(
      "node-exposure",
      "adjust.exposure",
      Json::array({"node-source"}),
      Json{{"ev", 1.25}});
  exposure["opacity"] = 0.75;
  exposure["mask"] = {
      {"maskId", "mask-subject"},
      {"contentHash", mask_content_hash()},
      {"inverted", false},
  };
  Json curve = node_json(
      "node-curve",
      "adjust.curve.rgb",
      Json::array({"node-exposure"}),
      Json{{
          "points",
          Json::array({
              Json{{"x", 0.0}, {"y", 0.05}},
              Json{{"x", 0.4}, {"y", 0.48}},
              Json{{"x", 1.0}, {"y", 0.95}},
          }),
      }});
  Json output = node_json(
      "node-output",
      "output",
      Json::array({"node-curve"}),
      Json::object());

  // Deliberately not nodeId order. The domain object normalizes storage.
  return Json{
      {"schema", "nps.edit-graph/v1"},
      {"graphId", "graph-main"},
      {"workingColorSpace",
       "nps.color/scene-linear-rec2020-d65/v1"},
      {"sourceNodeId", "node-source"},
      {"outputNodeId", "node-output"},
      {"nodes",
       Json::array({
           std::move(output),
           std::move(exposure),
           std::move(source),
           std::move(curve),
       })},
  };
}

[[nodiscard]] Json& find_node_json(Json& graph, std::string_view node_id) {
  for (Json& node : graph.at("nodes")) {
    if (node.at("nodeId").get_ref<const std::string&>() == node_id) {
      return node;
    }
  }
  throw std::logic_error("The synthetic test graph is missing a node.");
}

[[nodiscard]] EditGraph graph_from_text(std::string_view json_text) {
  EditGraphResult parsed =
      nps::document::parse_edit_graph_json(json_text);
  if (const auto* graph = std::get_if<EditGraph>(&parsed)) {
    return *graph;
  }
  throw std::logic_error("Expected the synthetic edit graph to be valid.");
}

[[nodiscard]] EditGraph graph_from(const Json& value) {
  return graph_from_text(value.dump());
}

[[nodiscard]] EditGraphError error_from_text(std::string_view json_text) {
  EditGraphResult parsed =
      nps::document::parse_edit_graph_json(json_text);
  if (const auto* error = std::get_if<EditGraphError>(&parsed)) {
    return *error;
  }
  throw std::logic_error("Expected the synthetic edit graph to be invalid.");
}

[[nodiscard]] EditGraphError error_from(const Json& value) {
  return error_from_text(value.dump());
}

[[nodiscard]] EditNode source_node(std::string id = "node-source") {
  return EditNode{
      .node_id = std::move(id),
      .kind = EditNodeKind::source,
      .algorithm_version = "1.0.0",
      .enabled = true,
      .opacity = 1.0,
      .compute_domain = ComputeDomain::scene_linear,
      .inputs = {},
      .parameters = SourceParameters{},
      .mask = std::nullopt,
  };
}

[[nodiscard]] EditNode exposure_node(
    std::string id = "node-exposure",
    std::string input = "node-source",
    double ev = 1.25) {
  return EditNode{
      .node_id = std::move(id),
      .kind = EditNodeKind::adjust_exposure,
      .algorithm_version = "1.0.0",
      .enabled = true,
      .opacity = 0.75,
      .compute_domain = ComputeDomain::scene_linear,
      .inputs = {std::move(input)},
      .parameters = ExposureParameters{.ev = ev},
      .mask = MaskBinding{
          .mask_id = "mask-subject",
          .content_hash = mask_content_hash(),
          .inverted = false,
      },
  };
}

[[nodiscard]] EditNode curve_node(
    std::string id = "node-curve",
    std::string input = "node-exposure") {
  return EditNode{
      .node_id = std::move(id),
      .kind = EditNodeKind::adjust_curve_rgb,
      .algorithm_version = "1.0.0",
      .enabled = true,
      .opacity = 1.0,
      .compute_domain = ComputeDomain::scene_linear,
      .inputs = {std::move(input)},
      .parameters = RgbCurveParameters{
          .points = {
              CurveControlPoint{.x = 0.0, .y = 0.05},
              CurveControlPoint{.x = 0.4, .y = 0.48},
              CurveControlPoint{.x = 1.0, .y = 0.95},
          },
      },
      .mask = std::nullopt,
  };
}

[[nodiscard]] EditNode output_node(
    std::string id = "node-output",
    std::string input = "node-curve") {
  return EditNode{
      .node_id = std::move(id),
      .kind = EditNodeKind::output,
      .algorithm_version = "1.0.0",
      .enabled = true,
      .opacity = 1.0,
      .compute_domain = ComputeDomain::scene_linear,
      .inputs = {std::move(input)},
      .parameters = OutputParameters{},
      .mask = std::nullopt,
  };
}

[[nodiscard]] EditGraphDefinition valid_definition() {
  return EditGraphDefinition{
      .graph_id = "graph-main",
      .working_color_space =
          "nps.color/scene-linear-rec2020-d65/v1",
      .source_node_id = "node-source",
      .output_node_id = "node-output",
      .nodes = {
          output_node(),
          exposure_node(),
          source_node(),
          curve_node(),
      },
  };
}

[[nodiscard]] EditGraphError create_error(EditGraphDefinition definition) {
  EditGraphResult result =
      nps::document::create_edit_graph(std::move(definition));
  if (const auto* error = std::get_if<EditGraphError>(&result)) {
    return *error;
  }
  throw std::logic_error("Expected the graph definition to be invalid.");
}

}  // namespace

TEST_CASE("strict edit graph parses, normalizes, and round trips") {
  const EditGraph graph = graph_from(valid_graph_json());

  CHECK(graph.graph_id() == "graph-main");
  CHECK(
      graph.working_color_space() ==
      nps::color::scene_linear_rec2020_d65_id);
  CHECK(graph.source_node_id() == "node-source");
  CHECK(graph.output_node_id() == "node-output");
  REQUIRE(graph.nodes().size() == 4U);
  CHECK(graph.nodes()[0].node_id == "node-curve");
  CHECK(graph.nodes()[1].node_id == "node-exposure");
  CHECK(graph.nodes()[2].node_id == "node-output");
  CHECK(graph.nodes()[3].node_id == "node-source");

  const EditNode* exposure = graph.find_node("node-exposure");
  REQUIRE(exposure != nullptr);
  REQUIRE(
      std::holds_alternative<ExposureParameters>(exposure->parameters));
  CHECK(std::get<ExposureParameters>(exposure->parameters).ev == 1.25);
  REQUIRE(exposure->mask.has_value());
  CHECK(exposure->mask->mask_id == "mask-subject");
  CHECK(exposure->mask->content_hash == mask_content_hash());
  CHECK_FALSE(exposure->mask->inverted);
  CHECK(graph.find_node("node-absent") == nullptr);

  const std::vector<const EditNode*> topological =
      graph.nodes_in_topological_order();
  REQUIRE(topological.size() == 4U);
  CHECK(topological[0]->node_id == "node-source");
  CHECK(topological[1]->node_id == "node-exposure");
  CHECK(topological[2]->node_id == "node-curve");
  CHECK(topological[3]->node_id == "node-output");

  const Json serialized = nps::document::edit_graph_to_json(graph);
  const EditGraph reparsed = graph_from(serialized);
  CHECK(reparsed == graph);
  CHECK(
      nps::document::canonical_edit_graph_json(reparsed) ==
      nps::document::canonical_edit_graph_json(graph));
}

TEST_CASE("canonical graph hash ignores non-semantic JSON ordering") {
  Json reordered = valid_graph_json();
  std::reverse(
      reordered.at("nodes").begin(),
      reordered.at("nodes").end());

  const EditGraph first = graph_from(valid_graph_json());
  const EditGraph second = graph_from(reordered);
  const std::string first_hash =
      nps::document::edit_graph_sha256(first);
  const std::string second_hash =
      nps::document::edit_graph_sha256(second);

  CHECK(first == second);
  CHECK(first_hash == second_hash);
  CHECK(first_hash.size() == 64U);
  CHECK(std::ranges::all_of(first_hash, [](char character) {
    return (character >= '0' && character <= '9') ||
           (character >= 'a' && character <= 'f');
  }));

  Json negative_zero = valid_graph_json();
  find_node_json(negative_zero, "node-exposure")["parameters"]["ev"] =
      -0.0;
  Json positive_zero = negative_zero;
  find_node_json(positive_zero, "node-exposure")["parameters"]["ev"] =
      0.0;
  CHECK(
      nps::document::edit_graph_sha256(graph_from(negative_zero)) ==
      nps::document::edit_graph_sha256(graph_from(positive_zero)));
}

TEST_CASE(
    "deterministic generated chains round trip and invalid variants never "
    "commit") {
  std::mt19937_64 random{0x4e50532d4d312d31ULL};
  for (std::size_t case_index = 0U; case_index < 128U; ++case_index) {
    const std::size_t adjustment_count =
        2U + static_cast<std::size_t>(random() % 23U);
    CAPTURE(case_index, adjustment_count);

    EditGraphDefinition definition{
        .graph_id = "graph-generated-" + std::to_string(case_index),
        .working_color_space =
            "nps.color/scene-linear-rec2020-d65/v1",
        .source_node_id = "node-source",
        .output_node_id = "node-output",
        .nodes = {source_node()},
    };
    std::string input = "node-source";
    std::string first_adjustment_id;
    for (std::size_t index = 0U; index < adjustment_count; ++index) {
      const std::string node_id =
          "node-generated-" + std::to_string(case_index) + "-" +
          std::to_string(index);
      if (first_adjustment_id.empty()) {
        first_adjustment_id = node_id;
      }
      if ((random() & 1U) == 0U) {
        const double ev =
            static_cast<double>(
                static_cast<int>(random() % 81U) - 40) /
            4.0;
        EditNode node = exposure_node(node_id, input, ev);
        node.opacity =
            static_cast<double>(random() % 101U) / 100.0;
        node.enabled = (random() & 1U) != 0U;
        node.mask.reset();
        definition.nodes.push_back(std::move(node));
      } else {
        EditNode node = curve_node(node_id, input);
        node.opacity =
            static_cast<double>(random() % 101U) / 100.0;
        node.enabled = (random() & 1U) != 0U;
        auto& points =
            std::get<RgbCurveParameters>(node.parameters).points;
        points[1].y =
            static_cast<double>(random() % 101U) / 100.0;
        definition.nodes.push_back(std::move(node));
      }
      input = node_id;
    }
    const std::string last_adjustment_id = input;
    definition.nodes.push_back(output_node("node-output", input));
    std::shuffle(
        definition.nodes.begin(), definition.nodes.end(), random);

    EditGraphDefinition missing_input = definition;
    EditGraphDefinition cycle = definition;
    EditGraphResult result =
        nps::document::create_edit_graph(std::move(definition));
    REQUIRE(std::holds_alternative<EditGraph>(result));
    const EditGraph graph = std::get<EditGraph>(std::move(result));
    const auto order = graph.nodes_in_topological_order();
    REQUIRE(order.size() == adjustment_count + 2U);
    CHECK(order.front()->node_id == "node-source");
    CHECK(order.back()->node_id == "node-output");

    const std::string canonical =
        nps::document::canonical_edit_graph_json(graph);
    const EditGraph reparsed = graph_from_text(canonical);
    CHECK(reparsed == graph);
    CHECK(
        nps::document::edit_graph_sha256(reparsed) ==
        nps::document::edit_graph_sha256(graph));

    auto first_missing = std::ranges::find(
        missing_input.nodes,
        first_adjustment_id,
        &EditNode::node_id);
    REQUIRE(first_missing != missing_input.nodes.end());
    first_missing->inputs = {"node-not-present"};
    CHECK(
        create_error(std::move(missing_input)).code ==
        EditGraphErrorCode::missing_input);

    auto first_cycle = std::ranges::find(
        cycle.nodes, first_adjustment_id, &EditNode::node_id);
    REQUIRE(first_cycle != cycle.nodes.end());
    first_cycle->inputs = {last_adjustment_id};
    CHECK(
        create_error(std::move(cycle)).code ==
        EditGraphErrorCode::cycle_detected);
  }
}

TEST_CASE("builder composes an atomic change without mutating its base graph") {
  const EditGraph base = graph_from(valid_graph_json());
  const std::string base_hash = nps::document::edit_graph_sha256(base);

  EditGraphBuilder replace_builder = EditGraphBuilder::from_graph(base);
  EditNode changed_exposure = *base.find_node("node-exposure");
  std::get<ExposureParameters>(changed_exposure.parameters).ev = 2.0;
  REQUIRE(replace_builder.replace_node(
      "node-exposure", std::move(changed_exposure)));
  EditGraphResult replaced_result = replace_builder.build();
  REQUIRE(std::holds_alternative<EditGraph>(replaced_result));
  const EditGraph replaced = std::get<EditGraph>(replaced_result);
  CHECK(nps::document::edit_graph_sha256(replaced) != base_hash);
  CHECK(nps::document::edit_graph_sha256(base) == base_hash);

  EditGraphBuilder insert_builder = EditGraphBuilder::from_graph(base);
  EditNode inserted =
      exposure_node("node-exposure-final", "node-curve", -0.5);
  EditNode rewired_output = *base.find_node("node-output");
  rewired_output.inputs = {"node-exposure-final"};
  REQUIRE(insert_builder.add_node(std::move(inserted)));
  REQUIRE(insert_builder.replace_node(
      "node-output", std::move(rewired_output)));
  EditGraphResult inserted_result = insert_builder.build();
  REQUIRE(std::holds_alternative<EditGraph>(inserted_result));
  const EditGraph inserted_graph = std::get<EditGraph>(inserted_result);
  CHECK(inserted_graph.nodes_in_topological_order().size() == 5U);

  EditGraphBuilder remove_builder =
      EditGraphBuilder::from_graph(inserted_graph);
  EditNode restored_output = *inserted_graph.find_node("node-output");
  restored_output.inputs = {"node-curve"};
  REQUIRE(remove_builder.replace_node(
      "node-output", std::move(restored_output)));
  REQUIRE(remove_builder.remove_node("node-exposure-final"));
  EditGraphResult restored_result = remove_builder.build();
  REQUIRE(std::holds_alternative<EditGraph>(restored_result));
  CHECK(std::get<EditGraph>(restored_result) == base);

  EditGraphBuilder operation_checks = EditGraphBuilder::from_graph(base);
  CHECK_FALSE(operation_checks.add_node(source_node()));
  CHECK_FALSE(operation_checks.replace_node(
      "node-absent", exposure_node()));
  CHECK_FALSE(operation_checks.replace_node(
      "node-exposure", exposure_node("different-id")));
  CHECK_FALSE(operation_checks.remove_node("node-absent"));
}

TEST_CASE("JSON envelope and every object boundary are strict") {
  SECTION("malformed JSON") {
    const EditGraphError error = error_from_text("{\"schema\":");
    CHECK(error.code == EditGraphErrorCode::invalid_json);
  }

  SECTION("duplicate JSON property") {
    std::string json_text = valid_graph_json().dump();
    const std::size_t position = json_text.find("\"graphId\"");
    REQUIRE(position != std::string::npos);
    json_text.insert(position, "\"graphId\":\"graph-other\",");
    const EditGraphError error = error_from_text(json_text);
    CHECK(error.code == EditGraphErrorCode::schema_invalid);
  }

  SECTION("unknown root property") {
    Json input = valid_graph_json();
    input["previewCache"] = true;
    CHECK(
        error_from(input).code ==
        EditGraphErrorCode::schema_invalid);
  }

  SECTION("unknown node property") {
    Json input = valid_graph_json();
    find_node_json(input, "node-exposure")["uiState"] = true;
    CHECK(
        error_from(input).code ==
        EditGraphErrorCode::schema_invalid);
  }

  SECTION("unknown parameter property") {
    Json input = valid_graph_json();
    find_node_json(input, "node-exposure")["parameters"]["automatic"] =
        true;
    CHECK(
        error_from(input).code ==
        EditGraphErrorCode::schema_invalid);
  }

  SECTION("unknown curve point property") {
    Json input = valid_graph_json();
    find_node_json(input, "node-curve")["parameters"]["points"][0]
        ["tangent"] = 0.0;
    CHECK(
        error_from(input).code ==
        EditGraphErrorCode::schema_invalid);
  }

  SECTION("unknown mask property") {
    Json input = valid_graph_json();
    find_node_json(input, "node-exposure")["mask"]["feather"] = 1.0;
    CHECK(
        error_from(input).code ==
        EditGraphErrorCode::schema_invalid);
  }

  SECTION("missing required property") {
    Json input = valid_graph_json();
    input.erase("sourceNodeId");
    CHECK(
        error_from(input).code ==
        EditGraphErrorCode::schema_invalid);
  }
}

TEST_CASE("graph schema, color space, identifiers, and registry are strict") {
  SECTION("wrong schema") {
    Json input = valid_graph_json();
    input["schema"] = "nps.edit-graph/v2";
    CHECK(
        error_from(input).code ==
        EditGraphErrorCode::schema_invalid);
  }

  SECTION("wrong working color space") {
    Json input = valid_graph_json();
    input["workingColorSpace"] =
        "nps.color/display-srgb-d65/v1";
    CHECK(
        error_from(input).code ==
        EditGraphErrorCode::schema_invalid);
  }

  SECTION("invalid graph identifier") {
    Json input = valid_graph_json();
    input["graphId"] = "graph with spaces";
    CHECK(
        error_from(input).code ==
        EditGraphErrorCode::invalid_identifier);
  }

  SECTION("invalid node identifier") {
    Json input = valid_graph_json();
    find_node_json(input, "node-exposure")["nodeId"] = "../node";
    CHECK(
        error_from(input).code ==
        EditGraphErrorCode::invalid_identifier);
  }

  SECTION("unknown critical node") {
    Json input = valid_graph_json();
    find_node_json(input, "node-exposure")["type"] =
        "critical.unregistered";
    CHECK(
        error_from(input).code ==
        EditGraphErrorCode::unknown_critical_node);
  }

  SECTION("unsupported algorithm version") {
    Json input = valid_graph_json();
    find_node_json(input, "node-exposure")["algorithmVersion"] =
        "2.0.0";
    CHECK(
        error_from(input).code ==
        EditGraphErrorCode::unsupported_algorithm_version);
  }

  SECTION("unknown programmatic node enum") {
    EditGraphDefinition definition = valid_definition();
    definition.nodes[1].kind = static_cast<EditNodeKind>(999);
    CHECK(
        create_error(std::move(definition)).code ==
        EditGraphErrorCode::unknown_critical_node);
  }
}

TEST_CASE("source and output node invariants are enforced") {
  SECTION("duplicate node identifier") {
    EditGraphDefinition definition = valid_definition();
    definition.nodes.push_back(exposure_node());
    CHECK(
        create_error(std::move(definition)).code ==
        EditGraphErrorCode::duplicate_node_id);
  }

  SECTION("multiple sources") {
    EditGraphDefinition definition = valid_definition();
    definition.nodes.push_back(source_node("node-source-two"));
    CHECK(
        create_error(std::move(definition)).code ==
        EditGraphErrorCode::invalid_source);
  }

  SECTION("source declaration points to adjustment") {
    Json input = valid_graph_json();
    input["sourceNodeId"] = "node-exposure";
    CHECK(
        error_from(input).code ==
        EditGraphErrorCode::invalid_source);
  }

  SECTION("source cannot be disabled") {
    Json input = valid_graph_json();
    find_node_json(input, "node-source")["enabled"] = false;
    CHECK(
        error_from(input).code ==
        EditGraphErrorCode::invalid_source);
  }

  SECTION("source cannot have an input") {
    Json input = valid_graph_json();
    find_node_json(input, "node-source")["inputs"] =
        Json::array({"node-output"});
    CHECK(
        error_from(input).code ==
        EditGraphErrorCode::invalid_source);
  }

  SECTION("missing output") {
    Json input = valid_graph_json();
    auto& nodes = input.at("nodes");
    const auto found = std::find_if(
        nodes.begin(), nodes.end(), [](const Json& node) {
          return node.at("nodeId") == "node-output";
        });
    REQUIRE(found != nodes.end());
    nodes.erase(found);
    CHECK(
        error_from(input).code ==
        EditGraphErrorCode::invalid_output);
  }

  SECTION("output declaration points to adjustment") {
    Json input = valid_graph_json();
    input["outputNodeId"] = "node-exposure";
    CHECK(
        error_from(input).code ==
        EditGraphErrorCode::invalid_output);
  }

  SECTION("output must remain fully opaque") {
    Json input = valid_graph_json();
    find_node_json(input, "node-output")["opacity"] = 0.5;
    CHECK(
        error_from(input).code ==
        EditGraphErrorCode::invalid_output);
  }
}

TEST_CASE("DAG references, cycles, and connectivity are enforced") {
  SECTION("missing input") {
    Json input = valid_graph_json();
    find_node_json(input, "node-exposure")["inputs"][0] =
        "node-missing";
    CHECK(
        error_from(input).code ==
        EditGraphErrorCode::missing_input);
  }

  SECTION("cycle") {
    Json input = valid_graph_json();
    find_node_json(input, "node-exposure")["inputs"][0] =
        "node-curve";
    CHECK(
        error_from(input).code ==
        EditGraphErrorCode::cycle_detected);
  }

  SECTION("adjustment requires an input") {
    Json input = valid_graph_json();
    find_node_json(input, "node-exposure")["inputs"] = Json::array();
    CHECK(
        error_from(input).code ==
        EditGraphErrorCode::invalid_topology);
  }

  SECTION("M1 adjustment rejects multiple inputs") {
    Json input = valid_graph_json();
    find_node_json(input, "node-exposure")["inputs"].push_back(
        "node-source");
    CHECK(
        error_from(input).code ==
        EditGraphErrorCode::invalid_topology);
  }

  SECTION("disconnected branch") {
    EditGraphDefinition definition = valid_definition();
    definition.nodes.push_back(
        exposure_node("node-unused", "node-source", 0.25));
    CHECK(
        create_error(std::move(definition)).code ==
        EditGraphErrorCode::invalid_topology);
  }
}

TEST_CASE("compute domain, opacity, and mask bindings are validated") {
  SECTION("unknown JSON compute domain") {
    Json input = valid_graph_json();
    find_node_json(input, "node-exposure")["computeDomain"] =
        "display-referred";
    CHECK(
        error_from(input).code ==
        EditGraphErrorCode::invalid_compute_domain);
  }

  SECTION("unknown programmatic compute domain") {
    EditGraphDefinition definition = valid_definition();
    definition.nodes[1].compute_domain =
        static_cast<ComputeDomain>(999);
    CHECK(
        create_error(std::move(definition)).code ==
        EditGraphErrorCode::invalid_compute_domain);
  }

  SECTION("opacity outside normalized interval") {
    Json input = valid_graph_json();
    find_node_json(input, "node-exposure")["opacity"] = 1.01;
    CHECK(
        error_from(input).code ==
        EditGraphErrorCode::invalid_opacity);
  }

  SECTION("non-finite programmatic opacity") {
    EditGraphDefinition definition = valid_definition();
    definition.nodes[1].opacity =
        std::numeric_limits<double>::infinity();
    CHECK(
        create_error(std::move(definition)).code ==
        EditGraphErrorCode::invalid_opacity);
  }

  SECTION("invalid mask identifier") {
    Json input = valid_graph_json();
    find_node_json(input, "node-exposure")["mask"]["maskId"] =
        "mask/private value";
    CHECK(
        error_from(input).code ==
        EditGraphErrorCode::invalid_mask);
  }

  SECTION("mask boolean has strict type") {
    Json input = valid_graph_json();
    find_node_json(input, "node-exposure")["mask"]["inverted"] = 0;
    CHECK(
        error_from(input).code ==
        EditGraphErrorCode::invalid_mask);
  }

  SECTION("mask content hash is explicit lowercase SHA-256") {
    Json input = valid_graph_json();
    find_node_json(input, "node-exposure")["mask"]["contentHash"] =
        std::string(64U, 'G');
    CHECK(
        error_from(input).code ==
        EditGraphErrorCode::invalid_mask);

    EditGraphDefinition definition = valid_definition();
    definition.nodes[1].mask->content_hash = "not-a-hash";
    CHECK(
        create_error(std::move(definition)).code ==
        EditGraphErrorCode::invalid_mask);
  }

  SECTION("source cannot bind a mask") {
    Json input = valid_graph_json();
    find_node_json(input, "node-source")["mask"] = {
        {"maskId", "mask-source"},
        {"contentHash", mask_content_hash()},
        {"inverted", false},
    };
    CHECK(
        error_from(input).code ==
        EditGraphErrorCode::invalid_mask);
  }

  SECTION("output cannot bind a mask") {
    Json input = valid_graph_json();
    find_node_json(input, "node-output")["mask"] = {
        {"maskId", "mask-output"},
        {"contentHash", mask_content_hash()},
        {"inverted", false},
    };
    CHECK(
        error_from(input).code ==
        EditGraphErrorCode::invalid_mask);
  }
}

TEST_CASE("exposure and parameter variants are validated") {
  SECTION("exposure below range") {
    Json input = valid_graph_json();
    find_node_json(input, "node-exposure")["parameters"]["ev"] = -10.01;
    CHECK(
        error_from(input).code ==
        EditGraphErrorCode::invalid_parameters);
  }

  SECTION("exposure above range") {
    Json input = valid_graph_json();
    find_node_json(input, "node-exposure")["parameters"]["ev"] = 10.01;
    CHECK(
        error_from(input).code ==
        EditGraphErrorCode::invalid_parameters);
  }

  SECTION("exposure wrong JSON type") {
    Json input = valid_graph_json();
    find_node_json(input, "node-exposure")["parameters"]["ev"] = "1.0";
    CHECK(
        error_from(input).code ==
        EditGraphErrorCode::invalid_parameters);
  }

  SECTION("non-finite programmatic exposure") {
    EditGraphDefinition definition = valid_definition();
    std::get<ExposureParameters>(definition.nodes[1].parameters).ev =
        std::numeric_limits<double>::quiet_NaN();
    CHECK(
        create_error(std::move(definition)).code ==
        EditGraphErrorCode::invalid_parameters);
  }

  SECTION("node kind and parameter variant must agree") {
    EditGraphDefinition definition = valid_definition();
    definition.nodes[1].parameters = OutputParameters{};
    CHECK(
        create_error(std::move(definition)).code ==
        EditGraphErrorCode::invalid_parameters);
  }
}

TEST_CASE("RGB curve control points have a strict full-domain order") {
  SECTION("too few points") {
    Json input = valid_graph_json();
    find_node_json(input, "node-curve")["parameters"]["points"] =
        Json::array({Json{{"x", 0.0}, {"y", 0.0}}});
    CHECK(
        error_from(input).code ==
        EditGraphErrorCode::invalid_parameters);
  }

  SECTION("coordinate outside normalized interval") {
    Json input = valid_graph_json();
    find_node_json(input, "node-curve")["parameters"]["points"][1]["y"] =
        1.01;
    CHECK(
        error_from(input).code ==
        EditGraphErrorCode::invalid_parameters);
  }

  SECTION("x coordinates must be strictly increasing") {
    Json input = valid_graph_json();
    find_node_json(input, "node-curve")["parameters"]["points"][1]["x"] =
        0.0;
    CHECK(
        error_from(input).code ==
        EditGraphErrorCode::invalid_parameters);
  }

  SECTION("curve must start at input zero") {
    Json input = valid_graph_json();
    find_node_json(input, "node-curve")["parameters"]["points"][0]["x"] =
        0.01;
    CHECK(
        error_from(input).code ==
        EditGraphErrorCode::invalid_parameters);
  }

  SECTION("curve must end at input one") {
    Json input = valid_graph_json();
    auto& points =
        find_node_json(input, "node-curve")["parameters"]["points"];
    points[points.size() - 1U]["x"] = 0.99;
    CHECK(
        error_from(input).code ==
        EditGraphErrorCode::invalid_parameters);
  }

  SECTION("coordinate wrong JSON type") {
    Json input = valid_graph_json();
    find_node_json(input, "node-curve")["parameters"]["points"][0]["y"] =
        "0.0";
    CHECK(
        error_from(input).code ==
        EditGraphErrorCode::invalid_parameters);
  }
}

TEST_CASE("public parse errors do not echo private input") {
  const std::string private_value =
      std::string("C:") + std::string(1U, '\\') +
      "Users" + std::string(1U, '\\') + "private-subject";
  Json input = valid_graph_json();
  find_node_json(input, "node-exposure")["type"] = private_value;
  const std::string original_json = input.dump();

  const EditGraphError error = error_from_text(original_json);

  CHECK(error.code == EditGraphErrorCode::unknown_critical_node);
  CHECK(error.message.find(private_value) == std::string::npos);
  CHECK(error.message.find(original_json) == std::string::npos);
  CHECK(
      nps::document::to_string(error.code) ==
      "EDIT_GRAPH_CRITICAL_NODE_UNKNOWN");
}

TEST_CASE("edit graph parsing is size and structural-depth bounded") {
  SECTION("oversized input") {
    const std::string input(
        nps::document::kMaximumEditGraphJsonBytes + 1U, ' ');
    const EditGraphError error = error_from_text(input);
    CHECK(error.code == EditGraphErrorCode::invalid_json);
    CHECK_FALSE(error.message.empty());
  }

  SECTION("excessive structural nesting") {
    std::string input(
        nps::document::kMaximumEditGraphJsonNestingDepth + 1U, '[');
    input += '0';
    input.append(
        nps::document::kMaximumEditGraphJsonNestingDepth + 1U, ']');
    const EditGraphError error = error_from_text(input);
    CHECK(error.code == EditGraphErrorCode::invalid_json);
  }

  SECTION("brackets inside a JSON string do not consume depth") {
    const std::string input =
        "\"" +
        std::string(
            nps::document::kMaximumEditGraphJsonNestingDepth + 1U,
            '[') +
        "\"";
    const EditGraphError error = error_from_text(input);
    CHECK(error.code == EditGraphErrorCode::schema_invalid);
  }
}
