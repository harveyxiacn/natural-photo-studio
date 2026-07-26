#include "nps/core/command.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <variant>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

namespace {

using nps::core::AdjustExposureParameters;
using nps::core::Command;
using nps::core::CommandError;
using nps::core::CommandErrorCode;
using nps::core::CommandKind;
using nps::core::HistoryParameters;
using nps::core::parse_command_json;
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
}
