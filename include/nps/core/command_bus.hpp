#pragma once

#include <variant>

#include "nps/core/command.hpp"
#include "nps/core/project_store.hpp"

namespace nps::core {

using CommandExecutionResult = std::variant<CommitResult, CommandError>;

class CommandBus final {
 public:
  explicit CommandBus(ProjectStore& project) noexcept;

  [[nodiscard]] CommandExecutionResult execute(
      const Command& command,
      FaultPoint fault_point = FaultPoint::none) const;

 private:
  ProjectStore* project_;
};

}  // namespace nps::core
