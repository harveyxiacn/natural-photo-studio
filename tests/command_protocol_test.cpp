#include "nps/core/command.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

namespace {

using nps::core::AdjustExposureParameters;
using nps::core::Command;
using nps::core::CommandError;
using nps::core::CommandErrorCode;
using nps::core::CommandKind;
using nps::core::GraphReplaceParameters;
using nps::core::HistoryParameters;
using nps::core::parse_command_json;
using nps::document::EditGraph;
using nps::document::EditGraphError;
using Json = nlohmann::json;

[[nodiscard]] Json valid_exposure_command() {
    return Json{
        {"schema", "nps.command/v1"},
        {"commandId", "cmd-001"},
        {"idempotencyKey", "desktop-session:001"},
        {"documentId", "doc-001"},
        {"expectedRevision", 7},
        {"kind", "adjust.exposure"},
        {"params", {{"ev", 1.25}}},
        {"privacy",
         {
             {"network", "deny"},
             {"cloudInference", "deny"},
         }},
    };
}

[[nodiscard]] Json valid_history_command(std::string kind) {
    Json command = valid_exposure_command();
    command["kind"] = std::move(kind);
    command["params"] = Json::object();
    return command;
}

[[nodiscard]] Json edit_node(
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

[[nodiscard]] Json valid_edit_graph(
    std::string graph_id = "graph-main") {
    return Json{
        {"schema", "nps.edit-graph/v1"},
        {"graphId", std::move(graph_id)},
        {"workingColorSpace",
         "nps.color/scene-linear-rec2020-d65/v1"},
        {"sourceNodeId", "node-source"},
        {"outputNodeId", "node-output"},
        {"nodes",
         Json::array({
             edit_node(
                 "node-source",
                 "source",
                 Json::array(),
                 Json::object()),
             edit_node(
                 "node-output",
                 "output",
                 Json::array({"node-source"}),
                 Json::object()),
         })},
    };
}

[[nodiscard]] EditGraph parsed_edit_graph(const Json& graph_json) {
    nps::document::EditGraphResult parsed =
        nps::document::parse_edit_graph_json(graph_json.dump());
    if (const auto* graph = std::get_if<EditGraph>(&parsed)) {
        return *graph;
    }
    const EditGraphError& error = std::get<EditGraphError>(parsed);
    throw std::logic_error(
        "The synthetic command graph is invalid: " + error.message);
}

[[nodiscard]] Json valid_graph_replace_command(
    std::string graph_id = "graph-main") {
    Json graph = valid_edit_graph(std::move(graph_id));
    const EditGraph normalized = parsed_edit_graph(graph);
    Json command = valid_exposure_command();
    command["kind"] = "graph.replace";
    command["params"] = {
        {"graph", std::move(graph)},
        {"graphHash", nps::document::edit_graph_sha256(normalized)},
    };
    return command;
}

}  // namespace

TEST_CASE("strict exposure command parses and round trips") {
    const auto result = parse_command_json(valid_exposure_command().dump());

    REQUIRE(std::holds_alternative<Command>(result));
    const Command& command = std::get<Command>(result);
    CHECK(command.command_id == "cmd-001");
    CHECK(command.idempotency_key == "desktop-session:001");
    CHECK(command.document_id == "doc-001");
    CHECK(command.expected_revision == 7);
    CHECK(command.kind == CommandKind::AdjustExposure);
    REQUIRE(
        std::holds_alternative<AdjustExposureParameters>(command.parameters));
    CHECK(std::get<AdjustExposureParameters>(command.parameters).ev == 1.25);

    const Json serialized = nps::core::command_to_json(command);
    CHECK(serialized == valid_exposure_command());

    const auto reparsed = parse_command_json(serialized.dump());
    REQUIRE(std::holds_alternative<Command>(reparsed));
    CHECK(std::get<Command>(reparsed) == command);
}

TEST_CASE("history commands require an empty parameter object") {
    SECTION("undo") {
        const auto result =
            parse_command_json(valid_history_command("history.undo").dump());
        REQUIRE(std::holds_alternative<Command>(result));
        const Command& command = std::get<Command>(result);
        CHECK(command.kind == CommandKind::HistoryUndo);
        CHECK(std::holds_alternative<HistoryParameters>(command.parameters));
    }

    SECTION("redo") {
        const auto result =
            parse_command_json(valid_history_command("history.redo").dump());
        REQUIRE(std::holds_alternative<Command>(result));
        const Command& command = std::get<Command>(result);
        CHECK(command.kind == CommandKind::HistoryRedo);
        CHECK(std::holds_alternative<HistoryParameters>(command.parameters));
    }

    SECTION("history parameters reject unknown fields") {
        Json input = valid_history_command("history.undo");
        input["params"]["steps"] = 1;
        const auto result = parse_command_json(input.dump());
        REQUIRE(std::holds_alternative<CommandError>(result));
        CHECK(
            std::get<CommandError>(result).code ==
            CommandErrorCode::CmdSchemaInvalid);
    }
}

TEST_CASE("strict graph replacement parses normalizes and round trips") {
    const Json input = valid_graph_replace_command();
    const auto result = parse_command_json(input.dump());

    REQUIRE(std::holds_alternative<Command>(result));
    const Command& command = std::get<Command>(result);
    CHECK(command.kind == CommandKind::GraphReplace);
    REQUIRE(
        std::holds_alternative<GraphReplaceParameters>(
            command.parameters));
    const GraphReplaceParameters& parameters =
        std::get<GraphReplaceParameters>(command.parameters);
    CHECK(
        parameters.graph_hash ==
        nps::document::edit_graph_sha256(parameters.graph));

    const Json serialized = nps::core::command_to_json(command);
    CHECK(serialized.at("params").at("graph") ==
          nps::document::edit_graph_to_json(parameters.graph));
    CHECK(serialized.at("params").at("graphHash") ==
          parameters.graph_hash);

    const auto reparsed = parse_command_json(serialized.dump());
    REQUIRE(std::holds_alternative<Command>(reparsed));
    CHECK(std::get<Command>(reparsed) == command);
}

TEST_CASE("graph replacement rejects invalid graphs hashes and fields") {
    SECTION("unknown graph property") {
        Json input = valid_graph_replace_command();
        input["params"]["graph"]["previewCache"] = true;
        const auto result = parse_command_json(input.dump());
        REQUIRE(std::holds_alternative<CommandError>(result));
        CHECK(
            std::get<CommandError>(result).code ==
            CommandErrorCode::CmdSchemaInvalid);
    }

    SECTION("uppercase hash") {
        Json input = valid_graph_replace_command();
        std::string hash =
            input["params"]["graphHash"].get<std::string>();
        hash.front() = 'A';
        input["params"]["graphHash"] = std::move(hash);
        const auto result = parse_command_json(input.dump());
        REQUIRE(std::holds_alternative<CommandError>(result));
        CHECK(
            std::get<CommandError>(result).code ==
            CommandErrorCode::CmdSchemaInvalid);
    }

    SECTION("canonical hash mismatch") {
        Json input = valid_graph_replace_command();
        input["params"]["graphHash"] = std::string(64U, '0');
        const auto result = parse_command_json(input.dump());
        REQUIRE(std::holds_alternative<CommandError>(result));
        CHECK(
            std::get<CommandError>(result).code ==
            CommandErrorCode::CmdSchemaInvalid);
    }

    SECTION("unknown graph replace parameter") {
        Json input = valid_graph_replace_command();
        input["params"]["merge"] = true;
        const auto result = parse_command_json(input.dump());
        REQUIRE(std::holds_alternative<CommandError>(result));
        CHECK(
            std::get<CommandError>(result).code ==
            CommandErrorCode::CmdSchemaInvalid);
    }

    SECTION("duplicate embedded graph property") {
        std::string input = valid_graph_replace_command().dump();
        const std::size_t position = input.find("\"graphId\"");
        REQUIRE(position != std::string::npos);
        input.insert(position, "\"graphId\":\"graph-other\",");
        const auto result = parse_command_json(input);
        REQUIRE(std::holds_alternative<CommandError>(result));
        CHECK(
            std::get<CommandError>(result).code ==
            CommandErrorCode::CmdSchemaInvalid);
    }
}

TEST_CASE("unknown fields are rejected at every object boundary") {
    SECTION("envelope") {
        Json input = valid_exposure_command();
        input["execution"] = {{"mode", "commit"}};
        const auto result = parse_command_json(input.dump());
        REQUIRE(std::holds_alternative<CommandError>(result));
        const CommandError& error = std::get<CommandError>(result);
        CHECK(error.code == CommandErrorCode::CmdSchemaInvalid);
        CHECK_FALSE(error.project_modified);
    }

    SECTION("exposure params") {
        Json input = valid_exposure_command();
        input["params"]["automatic"] = true;
        const auto result = parse_command_json(input.dump());
        REQUIRE(std::holds_alternative<CommandError>(result));
        CHECK(
            std::get<CommandError>(result).code ==
            CommandErrorCode::CmdSchemaInvalid);
    }

    SECTION("privacy") {
        Json input = valid_exposure_command();
        input["privacy"]["telemetry"] = "deny";
        const auto result = parse_command_json(input.dump());
        REQUIRE(std::holds_alternative<CommandError>(result));
        CHECK(
            std::get<CommandError>(result).code ==
            CommandErrorCode::CmdSchemaInvalid);
    }
}

TEST_CASE("schema and identifier grammar are strict") {
    SECTION("wrong schema") {
        Json input = valid_exposure_command();
        input["schema"] = "nps.command/v2";
        const auto result = parse_command_json(input.dump());
        REQUIRE(std::holds_alternative<CommandError>(result));
        CHECK(
            std::get<CommandError>(result).code ==
            CommandErrorCode::CmdSchemaInvalid);
    }

    SECTION("missing required field") {
        Json input = valid_exposure_command();
        input.erase("documentId");
        const auto result = parse_command_json(input.dump());
        REQUIRE(std::holds_alternative<CommandError>(result));
        CHECK(
            std::get<CommandError>(result).code ==
            CommandErrorCode::CmdSchemaInvalid);
    }

    SECTION("identifier contains whitespace") {
        Json input = valid_exposure_command();
        input["commandId"] = "cmd private";
        const auto result = parse_command_json(input.dump());
        REQUIRE(std::holds_alternative<CommandError>(result));
        CHECK(
            std::get<CommandError>(result).code ==
            CommandErrorCode::CmdSchemaInvalid);
    }

    SECTION("malformed JSON") {
        const auto result = parse_command_json("{\"schema\":");
        REQUIRE(std::holds_alternative<CommandError>(result));
        CHECK(
            std::get<CommandError>(result).code ==
            CommandErrorCode::CmdSchemaInvalid);
    }
}

TEST_CASE("command parsing is size depth and privacy bounded") {
    SECTION("empty input") {
        const auto result =
            parse_command_json(std::string_view{});
        REQUIRE(std::holds_alternative<CommandError>(result));
        CHECK(
            std::get<CommandError>(result).code ==
            CommandErrorCode::CmdSchemaInvalid);
    }

    SECTION("oversized input") {
        const std::string input(
            nps::core::kMaximumCommandJsonBytes + 1U, ' ');
        const auto result = parse_command_json(input);
        REQUIRE(std::holds_alternative<CommandError>(result));
        const CommandError& error = std::get<CommandError>(result);
        CHECK(error.code == CommandErrorCode::CmdSchemaInvalid);
        CHECK(error.message.find("bounded JSON") != std::string::npos);
    }

    SECTION("excessive structural nesting") {
        std::string input(
            nps::core::kMaximumCommandJsonNestingDepth + 1U, '[');
        input += '0';
        input.append(
            nps::core::kMaximumCommandJsonNestingDepth + 1U, ']');
        const auto result = parse_command_json(input);
        REQUIRE(std::holds_alternative<CommandError>(result));
        const CommandError& error = std::get<CommandError>(result);
        CHECK(error.code == CommandErrorCode::CmdSchemaInvalid);
        CHECK(error.message.find("bounded JSON") != std::string::npos);
    }

    SECTION("malformed input is never echoed") {
        const std::string private_marker = "private-subject-token";
        const auto result = parse_command_json(
            "{\"schema\":\"" + private_marker);
        REQUIRE(std::holds_alternative<CommandError>(result));
        const CommandError& error = std::get<CommandError>(result);
        CHECK(error.code == CommandErrorCode::CmdSchemaInvalid);
        CHECK(
            error.message.find(private_marker) ==
            std::string::npos);
    }

    SECTION("unknown envelope property names are never echoed") {
        const std::string private_marker =
            "private-subject-token-envelope";
        Json input = valid_exposure_command();
        input[private_marker] = true;
        const auto result = parse_command_json(input.dump());
        REQUIRE(std::holds_alternative<CommandError>(result));
        const CommandError& error = std::get<CommandError>(result);
        CHECK(error.code == CommandErrorCode::CmdSchemaInvalid);
        CHECK(error.message.find(private_marker) == std::string::npos);
    }

    SECTION("unknown params property names are never echoed") {
        const std::string private_marker =
            "private-subject-token-params";
        Json input = valid_exposure_command();
        input["params"][private_marker] = true;
        const auto result = parse_command_json(input.dump());
        REQUIRE(std::holds_alternative<CommandError>(result));
        const CommandError& error = std::get<CommandError>(result);
        CHECK(error.code == CommandErrorCode::CmdSchemaInvalid);
        CHECK(error.message.find(private_marker) == std::string::npos);
    }

    SECTION("unknown privacy property names are never echoed") {
        const std::string private_marker =
            "private-subject-token-privacy";
        Json input = valid_exposure_command();
        input["privacy"][private_marker] = "deny";
        const auto result = parse_command_json(input.dump());
        REQUIRE(std::holds_alternative<CommandError>(result));
        const CommandError& error = std::get<CommandError>(result);
        CHECK(error.code == CommandErrorCode::CmdSchemaInvalid);
        CHECK(error.message.find(private_marker) == std::string::npos);
    }
}

TEST_CASE("unknown command kinds use a stable unsupported error") {
    Json input = valid_exposure_command();
    input["kind"] = "object.remove";

    const auto result = parse_command_json(input.dump());

    REQUIRE(std::holds_alternative<CommandError>(result));
    const CommandError& error = std::get<CommandError>(result);
    CHECK(error.code == CommandErrorCode::CmdUnsupportedType);
    CHECK(nps::core::to_string(error.code) == "CMD_UNSUPPORTED_TYPE");
    CHECK_FALSE(error.project_modified);
}

TEST_CASE("expected revision is an exact non-negative JSON-safe integer") {
    SECTION("negative") {
        Json input = valid_exposure_command();
        input["expectedRevision"] = -1;
        const auto result = parse_command_json(input.dump());
        REQUIRE(std::holds_alternative<CommandError>(result));
        CHECK(
            std::get<CommandError>(result).code ==
            CommandErrorCode::CmdSchemaInvalid);
    }

    SECTION("fractional") {
        Json input = valid_exposure_command();
        input["expectedRevision"] = 7.5;
        const auto result = parse_command_json(input.dump());
        REQUIRE(std::holds_alternative<CommandError>(result));
        CHECK(
            std::get<CommandError>(result).code ==
            CommandErrorCode::CmdSchemaInvalid);
    }

    SECTION("larger than JSON safe integer") {
        Json input = valid_exposure_command();
        input["expectedRevision"] =
            nps::core::kMaxJsonSafeRevision + std::uint64_t{1};
        const auto result = parse_command_json(input.dump());
        REQUIRE(std::holds_alternative<CommandError>(result));
        CHECK(
            std::get<CommandError>(result).code ==
            CommandErrorCode::CmdSchemaInvalid);
    }
}

TEST_CASE("exposure EV is finite and constrained") {
    SECTION("below minimum") {
        Json input = valid_exposure_command();
        input["params"]["ev"] = -10.01;
        const auto result = parse_command_json(input.dump());
        REQUIRE(std::holds_alternative<CommandError>(result));
        CHECK(
            std::get<CommandError>(result).code ==
            CommandErrorCode::CmdSchemaInvalid);
    }

    SECTION("above maximum") {
        Json input = valid_exposure_command();
        input["params"]["ev"] = 10.01;
        const auto result = parse_command_json(input.dump());
        REQUIRE(std::holds_alternative<CommandError>(result));
        CHECK(
            std::get<CommandError>(result).code ==
            CommandErrorCode::CmdSchemaInvalid);
    }

    SECTION("wrong type") {
        Json input = valid_exposure_command();
        input["params"]["ev"] = "1.0";
        const auto result = parse_command_json(input.dump());
        REQUIRE(std::holds_alternative<CommandError>(result));
        CHECK(
            std::get<CommandError>(result).code ==
            CommandErrorCode::CmdSchemaInvalid);
    }
}

TEST_CASE("M0 privacy cannot enable network or cloud inference") {
    SECTION("network") {
        Json input = valid_exposure_command();
        input["privacy"]["network"] = "allow";
        const auto result = parse_command_json(input.dump());
        REQUIRE(std::holds_alternative<CommandError>(result));
        CHECK(
            std::get<CommandError>(result).code ==
            CommandErrorCode::CmdSchemaInvalid);
    }

    SECTION("cloud") {
        Json input = valid_exposure_command();
        input["privacy"]["cloudInference"] = "allow";
        const auto result = parse_command_json(input.dump());
        REQUIRE(std::holds_alternative<CommandError>(result));
        CHECK(
            std::get<CommandError>(result).code ==
            CommandErrorCode::CmdSchemaInvalid);
    }
}

TEST_CASE("canonical idempotency payload excludes transport identifiers") {
    const auto parsed =
        parse_command_json(valid_exposure_command().dump());
    REQUIRE(std::holds_alternative<Command>(parsed));
    const Command first = std::get<Command>(parsed);

    Command retry = first;
    retry.command_id = "cmd-transport-retry";
    retry.idempotency_key = "another-lookup-key";

    CHECK(
        nps::core::canonical_idempotency_payload(first) ==
        nps::core::canonical_idempotency_payload(retry));
    CHECK(nps::core::has_same_idempotent_payload(first, retry));

    std::get<AdjustExposureParameters>(retry.parameters).ev = 2.0;
    CHECK_FALSE(nps::core::has_same_idempotent_payload(first, retry));
}

TEST_CASE("graph replacement idempotency includes canonical graph and hash") {
    const auto parsed =
        parse_command_json(valid_graph_replace_command().dump());
    REQUIRE(std::holds_alternative<Command>(parsed));
    const Command first = std::get<Command>(parsed);

    Command retry = first;
    retry.command_id = "cmd-graph-transport-retry";
    retry.idempotency_key = "another-graph-lookup-key";
    CHECK(nps::core::has_same_idempotent_payload(first, retry));

    Json reordered_input = valid_graph_replace_command();
    Json& reordered_nodes =
        reordered_input["params"]["graph"]["nodes"];
    reordered_nodes = Json::array({
        reordered_nodes.at(1),
        reordered_nodes.at(0),
    });
    const auto reordered_parsed =
        parse_command_json(reordered_input.dump());
    REQUIRE(std::holds_alternative<Command>(reordered_parsed));
    Command reordered = std::get<Command>(reordered_parsed);
    reordered.command_id = first.command_id;
    reordered.idempotency_key = first.idempotency_key;
    CHECK(nps::core::has_same_idempotent_payload(first, reordered));

    const Json payload =
        Json::parse(nps::core::canonical_idempotency_payload(first));
    const auto& first_parameters =
        std::get<GraphReplaceParameters>(first.parameters);
    CHECK(
        payload.at("params").at("graph") ==
        nps::document::edit_graph_to_json(first_parameters.graph));
    CHECK(
        payload.at("params").at("graphHash") ==
        first_parameters.graph_hash);

    const auto changed_parsed =
        parse_command_json(
            valid_graph_replace_command("graph-changed").dump());
    REQUIRE(std::holds_alternative<Command>(changed_parsed));
    Command changed = std::get<Command>(changed_parsed);
    changed.command_id = first.command_id;
    changed.idempotency_key = first.idempotency_key;
    CHECK_FALSE(nps::core::has_same_idempotent_payload(first, changed));
}

TEST_CASE("transaction-layer errors have stable codes and recovery context") {
    const CommandError reused =
        nps::core::make_idempotency_reuse_error("desktop-session:001");
    CHECK(reused.code == CommandErrorCode::CmdIdempotencyReuse);
    CHECK(nps::core::to_string(reused.code) == "CMD_IDEMPOTENCY_REUSE");
    CHECK_FALSE(reused.project_modified);
    CHECK_FALSE(reused.current_revision.has_value());

    const CommandError conflict =
        nps::core::make_revision_conflict_error(7, 8);
    CHECK(conflict.code == CommandErrorCode::RevisionConflict);
    CHECK(nps::core::to_string(conflict.code) == "REV_CONFLICT");
    CHECK_FALSE(conflict.project_modified);
    REQUIRE(conflict.current_revision.has_value());
    CHECK(*conflict.current_revision == 8);
}

TEST_CASE("programmatically constructed commands are validated") {
    Command invalid{
        .command_id = "cmd-001",
        .idempotency_key = "desktop-session:001",
        .document_id = "doc-001",
        .expected_revision = 7,
        .kind = CommandKind::HistoryUndo,
        .parameters = AdjustExposureParameters{.ev = 1.0},
        .privacy = {},
    };

    const auto validation_error = nps::core::validate_command(invalid);
    REQUIRE(validation_error.has_value());
    CHECK(validation_error->code == CommandErrorCode::CmdSchemaInvalid);
    CHECK_THROWS_AS(
        nps::core::command_to_json(invalid), std::invalid_argument);

    SECTION("graph hash must match the canonical C++ graph") {
        const EditGraph graph = parsed_edit_graph(valid_edit_graph());
        Command graph_command{
            .command_id = "cmd-graph",
            .idempotency_key = "desktop-session:graph",
            .document_id = "doc-001",
            .expected_revision = 7,
            .kind = CommandKind::GraphReplace,
            .parameters = GraphReplaceParameters{
                .graph = graph,
                .graph_hash =
                    nps::document::edit_graph_sha256(graph),
            },
            .privacy = {},
        };

        CHECK_FALSE(
            nps::core::validate_command(graph_command).has_value());

        std::get<GraphReplaceParameters>(
            graph_command.parameters).graph_hash = std::string(64U, 'f');
        const auto graph_error =
            nps::core::validate_command(graph_command);
        REQUIRE(graph_error.has_value());
        CHECK(
            graph_error->code ==
            CommandErrorCode::CmdSchemaInvalid);
        CHECK_THROWS_AS(
            nps::core::canonical_idempotency_payload(graph_command),
            std::invalid_argument);
    }
}
