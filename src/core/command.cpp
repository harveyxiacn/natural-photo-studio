#include "nps/core/command.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <ranges>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace nps::core {
namespace {

using Json = nlohmann::json;

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

[[nodiscard]] CommandError make_error(
    CommandErrorCode code,
    std::string detail,
    std::optional<std::uint64_t> current_revision = std::nullopt) {
    detail += " The project was not modified.";
    return CommandError{
        .code = code,
        .message = std::move(detail),
        .project_modified = false,
        .current_revision = current_revision,
    };
}

[[nodiscard]] CommandError schema_error(std::string detail) {
    return make_error(CommandErrorCode::CmdSchemaInvalid, std::move(detail));
}

[[nodiscard]] bool is_ascii_alpha_numeric(char value) noexcept {
    return (value >= 'a' && value <= 'z') ||
           (value >= 'A' && value <= 'Z') ||
           (value >= '0' && value <= '9');
}

[[nodiscard]] bool is_identifier_character(
    char value,
    bool allow_slash) noexcept {
    return is_ascii_alpha_numeric(value) || value == '.' || value == '_' ||
           value == ':' || value == '-' || (allow_slash && value == '/');
}

[[nodiscard]] std::optional<CommandError> validate_identifier(
    std::string_view value,
    std::string_view field_name,
    std::size_t maximum_length,
    bool allow_slash) {
    if (value.empty() || value.size() > maximum_length) {
        return schema_error(
            std::string(field_name) + " must contain between 1 and " +
            std::to_string(maximum_length) + " ASCII characters.");
    }

    if (!is_ascii_alpha_numeric(value.front()) ||
        !std::ranges::all_of(
            value,
            [allow_slash](char character) {
                return is_identifier_character(character, allow_slash);
            })) {
        return schema_error(
            std::string(field_name) +
            " contains characters outside the command identifier grammar.");
    }

    return std::nullopt;
}

[[nodiscard]] bool is_expected_key(
    std::string_view actual,
    std::initializer_list<std::string_view> expected) {
    return std::ranges::find(expected, actual) != expected.end();
}

[[nodiscard]] std::optional<CommandError> require_exact_keys(
    const Json& value,
    std::initializer_list<std::string_view> required,
    std::string_view object_name) {
    if (!value.is_object()) {
        return schema_error(
            std::string(object_name) + " must be a JSON object.");
    }

    for (const std::string_view required_key : required) {
        if (!value.contains(std::string(required_key))) {
            return schema_error(
                std::string(object_name) + " is missing required property '" +
                std::string(required_key) + "'.");
        }
    }

    for (const auto& [actual_key, ignored] : value.items()) {
        static_cast<void>(ignored);
        if (!is_expected_key(actual_key, required)) {
            return schema_error(
                std::string(object_name) +
                " contains an unsupported property.");
        }
    }

    return std::nullopt;
}

[[nodiscard]] std::optional<CommandError> validate_exposure_value(
    double ev) {
    if (!std::isfinite(ev) || ev < kMinExposureEv || ev > kMaxExposureEv) {
        return schema_error(
            "params.ev must be a finite number from -10 through 10 EV.");
    }
    return std::nullopt;
}

[[nodiscard]] double normalized_ev(double ev) noexcept {
    return ev == 0.0 ? 0.0 : ev;
}

[[nodiscard]] bool is_lowercase_sha256(std::string_view value) noexcept {
    return value.size() == 64U &&
           std::ranges::all_of(
               value,
               [](char character) {
                   return (character >= '0' && character <= '9') ||
                          (character >= 'a' && character <= 'f');
               });
}

[[nodiscard]] std::optional<CommandError>
validate_graph_replace_parameters(
    const GraphReplaceParameters& parameters) {
    if (!is_lowercase_sha256(parameters.graph_hash)) {
        return schema_error(
            "params.graphHash must be exactly 64 lowercase hexadecimal "
            "SHA-256 characters.");
    }

    try {
        const std::string canonical_graph =
            document::canonical_edit_graph_json(parameters.graph);
        document::EditGraphResult reparsed =
            document::parse_edit_graph_json(canonical_graph);
        if (!std::holds_alternative<document::EditGraph>(reparsed)) {
            return schema_error(
                "params.graph must be a valid strict "
                "nps.edit-graph/v1 object.");
        }

        const document::EditGraph& normalized_graph =
            std::get<document::EditGraph>(reparsed);
        if (document::edit_graph_sha256(normalized_graph) !=
            parameters.graph_hash) {
            return schema_error(
                "params.graphHash does not match the canonical edit graph.");
        }
    } catch (const std::exception&) {
        return schema_error(
            "params.graph could not be validated or content-addressed.");
    }

    return std::nullopt;
}

[[nodiscard]] Json privacy_to_json(const CommandPrivacy& privacy) {
    if (privacy.network != DataAccessPolicy::Deny ||
        privacy.cloud_inference != DataAccessPolicy::Deny) {
        throw std::invalid_argument(
            "Commands require network and cloud inference to be denied.");
    }

    return Json{
        {"network", "deny"},
        {"cloudInference", "deny"},
    };
}

[[nodiscard]] Json parameters_to_json(const Command& command) {
    switch (command.kind) {
        case CommandKind::AdjustExposure: {
            const auto* parameters =
                std::get_if<AdjustExposureParameters>(&command.parameters);
            if (parameters == nullptr) {
                throw std::invalid_argument(
                    "adjust.exposure requires AdjustExposureParameters.");
            }
            return Json{{"ev", normalized_ev(parameters->ev)}};
        }
        case CommandKind::HistoryUndo:
        case CommandKind::HistoryRedo:
            if (!std::holds_alternative<HistoryParameters>(
                    command.parameters)) {
                throw std::invalid_argument(
                    "history commands require HistoryParameters.");
            }
            return Json::object();
        case CommandKind::GraphReplace: {
            const auto* parameters =
                std::get_if<GraphReplaceParameters>(&command.parameters);
            if (parameters == nullptr) {
                throw std::invalid_argument(
                    "graph.replace requires GraphReplaceParameters.");
            }
            return Json{
                {"graph", document::edit_graph_to_json(parameters->graph)},
                {"graphHash", parameters->graph_hash},
            };
        }
    }

    throw std::invalid_argument("The command kind is not registered.");
}

}  // namespace

std::string_view to_string(CommandKind kind) noexcept {
    switch (kind) {
        case CommandKind::AdjustExposure:
            return "adjust.exposure";
        case CommandKind::HistoryUndo:
            return "history.undo";
        case CommandKind::HistoryRedo:
            return "history.redo";
        case CommandKind::GraphReplace:
            return "graph.replace";
    }
    return "<invalid-command-kind>";
}

std::optional<CommandKind> command_kind_from_string(
    std::string_view value) noexcept {
    if (value == "adjust.exposure") {
        return CommandKind::AdjustExposure;
    }
    if (value == "history.undo") {
        return CommandKind::HistoryUndo;
    }
    if (value == "history.redo") {
        return CommandKind::HistoryRedo;
    }
    if (value == "graph.replace") {
        return CommandKind::GraphReplace;
    }
    return std::nullopt;
}

std::string_view to_string(CommandErrorCode code) noexcept {
    switch (code) {
        case CommandErrorCode::CmdSchemaInvalid:
            return "CMD_SCHEMA_INVALID";
        case CommandErrorCode::CmdUnsupportedType:
            return "CMD_UNSUPPORTED_TYPE";
        case CommandErrorCode::CmdIdempotencyReuse:
            return "CMD_IDEMPOTENCY_REUSE";
        case CommandErrorCode::RevisionConflict:
            return "REV_CONFLICT";
    }
    return "INTERNAL_UNKNOWN_COMMAND_ERROR";
}

std::optional<CommandError> validate_command(const Command& command) {
    if (auto error = validate_identifier(
            command.command_id, "commandId", 128, false)) {
        return error;
    }
    if (auto error = validate_identifier(
            command.idempotency_key, "idempotencyKey", 256, true)) {
        return error;
    }
    if (auto error = validate_identifier(
            command.document_id, "documentId", 128, false)) {
        return error;
    }
    if (command.expected_revision > kMaxJsonSafeRevision) {
        return schema_error(
            "expectedRevision exceeds the maximum JSON-safe integer.");
    }
    if (command.privacy.network != DataAccessPolicy::Deny ||
        command.privacy.cloud_inference != DataAccessPolicy::Deny) {
        return schema_error(
            "privacy.network and privacy.cloudInference must both be 'deny' "
            "for local execution.");
    }

    switch (command.kind) {
        case CommandKind::AdjustExposure: {
            const auto* parameters =
                std::get_if<AdjustExposureParameters>(&command.parameters);
            if (parameters == nullptr) {
                return schema_error(
                    "adjust.exposure requires exactly an ev parameter.");
            }
            return validate_exposure_value(parameters->ev);
        }
        case CommandKind::HistoryUndo:
        case CommandKind::HistoryRedo:
            if (!std::holds_alternative<HistoryParameters>(
                    command.parameters)) {
                return schema_error(
                    "history.undo and history.redo require empty params.");
            }
            return std::nullopt;
        case CommandKind::GraphReplace: {
            const auto* parameters =
                std::get_if<GraphReplaceParameters>(&command.parameters);
            if (parameters == nullptr) {
                return schema_error(
                    "graph.replace requires exactly graph and graphHash "
                    "parameters.");
            }
            return validate_graph_replace_parameters(*parameters);
        }
    }

    return make_error(
        CommandErrorCode::CmdUnsupportedType,
        "The command kind is not registered. Choose a supported command kind.");
}

CommandParseResult parse_command_json(std::string_view json_text) {
    if (json_text.empty() ||
        json_text.size() > kMaximumCommandJsonBytes ||
        !json_nesting_is_bounded(
            json_text, kMaximumCommandJsonNestingDepth)) {
        return schema_error(
            "The command is not valid bounded JSON.");
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
            } else if (
                event == Json::parse_event_t::object_end &&
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
        return schema_error("The command is not valid bounded JSON.");
    }
    if (duplicate_property) {
        return schema_error(
            "Duplicate JSON object properties are not allowed.");
    }

    constexpr std::array<std::string_view, 8> envelope_keys{
        "schema",
        "commandId",
        "idempotencyKey",
        "documentId",
        "expectedRevision",
        "kind",
        "params",
        "privacy",
    };
    if (!root.is_object()) {
        return schema_error("The command envelope must be a JSON object.");
    }
    for (const std::string_view key : envelope_keys) {
        if (!root.contains(std::string(key))) {
            return schema_error(
                "The command envelope is missing required property '" +
                std::string(key) + "'.");
        }
    }
    for (const auto& [key, ignored] : root.items()) {
        static_cast<void>(ignored);
        const auto expected_key = std::ranges::find_if(
            envelope_keys,
            [&key](std::string_view candidate) {
                return candidate == std::string_view(key);
            });
        if (expected_key == envelope_keys.end()) {
            return schema_error(
                "The command envelope contains an unsupported property.");
        }
    }

    if (!root.at("schema").is_string() ||
        root.at("schema").get_ref<const std::string&>() != kCommandSchema) {
        return schema_error("schema must be exactly 'nps.command/v1'.");
    }

    for (const std::string_view key :
         {"commandId", "idempotencyKey", "documentId"}) {
        if (!root.at(std::string(key)).is_string()) {
            return schema_error(
                std::string(key) + " must be a JSON string.");
        }
    }

    if (!root.at("expectedRevision").is_number_integer()) {
        return schema_error(
            "expectedRevision must be a non-negative JSON integer.");
    }

    std::uint64_t expected_revision = 0;
    try {
        if (root.at("expectedRevision").is_number_unsigned()) {
            expected_revision =
                root.at("expectedRevision").get<std::uint64_t>();
        } else {
            const std::int64_t signed_revision =
                root.at("expectedRevision").get<std::int64_t>();
            if (signed_revision < 0) {
                return schema_error(
                    "expectedRevision must be a non-negative JSON integer.");
            }
            expected_revision = static_cast<std::uint64_t>(signed_revision);
        }
    } catch (const Json::exception&) {
        return schema_error(
            "expectedRevision is outside the supported integer range.");
    }
    if (expected_revision > kMaxJsonSafeRevision) {
        return schema_error(
            "expectedRevision exceeds the maximum JSON-safe integer.");
    }

    const Json& privacy = root.at("privacy");
    if (auto error = require_exact_keys(
            privacy, {"network", "cloudInference"}, "privacy")) {
        return *std::move(error);
    }
    for (const std::string_view key : {"network", "cloudInference"}) {
        const Json& policy = privacy.at(std::string(key));
        if (!policy.is_string() ||
            policy.get_ref<const std::string&>() != "deny") {
            return schema_error(
                "privacy." + std::string(key) +
                " must be exactly 'deny' for M0.");
        }
    }

    if (!root.at("kind").is_string()) {
        return schema_error("kind must be a JSON string.");
    }
    const std::string& kind_text =
        root.at("kind").get_ref<const std::string&>();
    const std::optional<CommandKind> kind =
        command_kind_from_string(kind_text);
    if (!kind.has_value()) {
        return make_error(
            CommandErrorCode::CmdUnsupportedType,
            "The requested command kind is not registered. Choose "
            "adjust.exposure, history.undo, history.redo, or graph.replace.");
    }

    const Json& parameters_json = root.at("params");
    CommandParameters parameters = HistoryParameters{};
    if (*kind == CommandKind::AdjustExposure) {
        if (auto error =
                require_exact_keys(parameters_json, {"ev"}, "params")) {
            return *std::move(error);
        }
        if (!parameters_json.at("ev").is_number()) {
            return schema_error("params.ev must be a JSON number.");
        }

        double ev = 0.0;
        try {
            ev = parameters_json.at("ev").get<double>();
        } catch (const Json::exception&) {
            return schema_error("params.ev is outside the supported range.");
        }
        if (auto error = validate_exposure_value(ev)) {
            return *std::move(error);
        }
        parameters = AdjustExposureParameters{.ev = normalized_ev(ev)};
    } else if (*kind == CommandKind::GraphReplace) {
        if (auto error = require_exact_keys(
                parameters_json,
                {"graph", "graphHash"},
                "params")) {
            return *std::move(error);
        }
        if (!parameters_json.at("graph").is_object()) {
            return schema_error(
                "params.graph must be a JSON object.");
        }
        if (!parameters_json.at("graphHash").is_string()) {
            return schema_error(
                "params.graphHash must be a JSON string.");
        }

        document::EditGraphResult parsed_graph =
            document::parse_edit_graph_json(
                parameters_json.at("graph").dump());
        if (!std::holds_alternative<document::EditGraph>(parsed_graph)) {
            return schema_error(
                "params.graph must be a valid strict "
                "nps.edit-graph/v1 object.");
        }

        parameters = GraphReplaceParameters{
            .graph =
                std::get<document::EditGraph>(std::move(parsed_graph)),
            .graph_hash =
                parameters_json.at("graphHash")
                    .get_ref<const std::string&>(),
        };
    } else {
        if (auto error = require_exact_keys(parameters_json, {}, "params")) {
            return *std::move(error);
        }
        parameters = HistoryParameters{};
    }

    Command command{
        .command_id =
            root.at("commandId").get_ref<const std::string&>(),
        .idempotency_key =
            root.at("idempotencyKey").get_ref<const std::string&>(),
        .document_id =
            root.at("documentId").get_ref<const std::string&>(),
        .expected_revision = expected_revision,
        .kind = *kind,
        .parameters = std::move(parameters),
        .privacy = {},
    };

    if (auto error = validate_command(command)) {
        return *std::move(error);
    }
    return command;
}

nlohmann::json command_to_json(const Command& command) {
    if (const auto error = validate_command(command)) {
        throw std::invalid_argument(error->message);
    }

    return Json{
        {"schema", std::string(kCommandSchema)},
        {"commandId", command.command_id},
        {"idempotencyKey", command.idempotency_key},
        {"documentId", command.document_id},
        {"expectedRevision", command.expected_revision},
        {"kind", std::string(to_string(command.kind))},
        {"params", parameters_to_json(command)},
        {"privacy", privacy_to_json(command.privacy)},
    };
}

std::string canonical_idempotency_payload(const Command& command) {
    if (const auto error = validate_command(command)) {
        throw std::invalid_argument(error->message);
    }

    const Json payload{
        {"schema", std::string(kCommandSchema)},
        {"documentId", command.document_id},
        {"expectedRevision", command.expected_revision},
        {"kind", std::string(to_string(command.kind))},
        {"params", parameters_to_json(command)},
        {"privacy", privacy_to_json(command.privacy)},
    };
    return payload.dump();
}

bool has_same_idempotent_payload(
    const Command& left,
    const Command& right) {
    return canonical_idempotency_payload(left) ==
           canonical_idempotency_payload(right);
}

CommandError make_idempotency_reuse_error(
    std::string_view idempotency_key) {
    return make_error(
        CommandErrorCode::CmdIdempotencyReuse,
        "Idempotency key '" + std::string(idempotency_key) +
            "' was already used for a different request. Use a new key or "
            "retry the original payload.");
}

CommandError make_revision_conflict_error(
    std::uint64_t expected_revision,
    std::uint64_t current_revision) {
    return make_error(
        CommandErrorCode::RevisionConflict,
        "expectedRevision " + std::to_string(expected_revision) +
            " does not match current revision " +
            std::to_string(current_revision) +
            ". Reload the current document revision and rebuild the command.",
        current_revision);
}

}  // namespace nps::core
