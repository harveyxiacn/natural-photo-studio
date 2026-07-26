#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

#include <nlohmann/json.hpp>

#include "nps/document/edit_graph.hpp"

namespace nps::core {

inline constexpr std::string_view kCommandSchema = "nps.command/v1";
inline constexpr std::uint64_t kMaxJsonSafeRevision = 9'007'199'254'740'991ULL;
inline constexpr double kMinExposureEv = -10.0;
inline constexpr double kMaxExposureEv = 10.0;
inline constexpr std::size_t kMaximumCommandJsonBytes =
    document::kMaximumEditGraphJsonBytes + 64U * 1024U;
inline constexpr std::size_t kMaximumCommandJsonNestingDepth = 64U;

enum class CommandKind {
    AdjustExposure,
    HistoryUndo,
    HistoryRedo,
    GraphReplace,
};

[[nodiscard]] std::string_view to_string(CommandKind kind) noexcept;
[[nodiscard]] std::optional<CommandKind> command_kind_from_string(
    std::string_view value) noexcept;

struct AdjustExposureParameters {
    double ev{0.0};

    [[nodiscard]] bool operator==(const AdjustExposureParameters&) const = default;
};

struct HistoryParameters {
    [[nodiscard]] bool operator==(const HistoryParameters&) const = default;
};

struct GraphReplaceParameters {
    document::EditGraph graph;
    std::string graph_hash;

    [[nodiscard]] bool operator==(const GraphReplaceParameters&) const =
        default;
};

using CommandParameters =
    std::variant<
        AdjustExposureParameters,
        HistoryParameters,
        GraphReplaceParameters>;

enum class DataAccessPolicy {
    Deny,
};

struct CommandPrivacy {
    DataAccessPolicy network{DataAccessPolicy::Deny};
    DataAccessPolicy cloud_inference{DataAccessPolicy::Deny};

    [[nodiscard]] bool operator==(const CommandPrivacy&) const = default;
};

struct Command {
    std::string command_id;
    std::string idempotency_key;
    std::string document_id;
    std::uint64_t expected_revision{0};
    CommandKind kind{CommandKind::AdjustExposure};
    CommandParameters parameters{AdjustExposureParameters{}};
    CommandPrivacy privacy{};

    [[nodiscard]] bool operator==(const Command&) const = default;
};

enum class CommandErrorCode {
    CmdSchemaInvalid,
    CmdUnsupportedType,
    CmdIdempotencyReuse,
    RevisionConflict,
};

[[nodiscard]] std::string_view to_string(CommandErrorCode code) noexcept;

struct CommandError {
    CommandErrorCode code{CommandErrorCode::CmdSchemaInvalid};
    std::string message;
    bool project_modified{false};
    std::optional<std::uint64_t> current_revision;

    [[nodiscard]] bool operator==(const CommandError&) const = default;
};

using CommandParseResult = std::variant<Command, CommandError>;

// Parses and validates the strict supported subset of nps.command/v1. No
// unknown or duplicate properties are accepted at any object level.
[[nodiscard]] CommandParseResult parse_command_json(std::string_view json_text);

// Validates commands constructed by C++ callers against the same invariants.
[[nodiscard]] std::optional<CommandError> validate_command(
    const Command& command);

// Serializes the complete command envelope in a stable, normalized form.
// Throws std::invalid_argument when command is invalid.
[[nodiscard]] nlohmann::json command_to_json(const Command& command);

// Returns normalized compact JSON for the idempotency request payload.
// commandId and idempotencyKey are intentionally excluded. The schema,
// document, expected revision, kind, parameters and privacy policy are included.
// Throws std::invalid_argument when command is invalid.
[[nodiscard]] std::string canonical_idempotency_payload(
    const Command& command);

[[nodiscard]] bool has_same_idempotent_payload(
    const Command& left,
    const Command& right);

[[nodiscard]] CommandError make_idempotency_reuse_error(
    std::string_view idempotency_key);

[[nodiscard]] CommandError make_revision_conflict_error(
    std::uint64_t expected_revision,
    std::uint64_t current_revision);

}  // namespace nps::core
