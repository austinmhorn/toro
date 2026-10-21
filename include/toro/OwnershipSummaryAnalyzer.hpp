#pragma once

#include "toro/IR.hpp"

#include <cstddef>
#include <optional>

namespace toro {

class OwnershipSummaryAnalyzer {
public:
    void analyze(IRProgram& program) const;
};

[[nodiscard]] const IRCallableOwnershipSummary* find_ownership_summary(
    const IRProgram& program,
    const IROperation& call);

[[nodiscard]] std::optional<IRReturnOwnershipSummary>
ownership_for_call_result(
    const IRProgram& program,
    const IROperation& call);

[[nodiscard]] std::optional<IRParameterOwnershipSummary>
ownership_for_call_operand(
    const IRProgram& program,
    const IROperation& call,
    std::size_t operand_index);

[[nodiscard]] bool ownership_is_observed_only(
    const IRParameterOwnershipSummary& summary);

[[nodiscard]] bool ownership_may_escape(
    const IRParameterOwnershipSummary& summary);

} // namespace toro
