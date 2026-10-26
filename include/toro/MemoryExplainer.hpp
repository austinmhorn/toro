#pragma once

#include "toro/MemoryReporter.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace toro {

enum class MemoryExplanationStatus {
    Explained,
    Ambiguous,
    NoMemoryDecision,
};

enum class MemoryExplanationKind {
    Allocation,
    Escape,
    ArcKept,
    ArcElided,
    ArcOperation,
    Borrow,
    Transfer,
    StrongField,
    WeakField,
    PossibleStrongCycle,
};

struct MemoryExplanationReason {
    std::string code;
    std::string detail;
    std::optional<IROrigin> origin;
    std::string related_identity;
};

struct MemoryExplanation {
    MemoryExplanationKind kind{MemoryExplanationKind::Allocation};
    IROrigin origin;
    std::string subject_identity;
    std::string decision;
    std::optional<IRType> type;
    std::optional<IRStorageClass> storage;
    std::optional<IREscapeState> escape_state;
    std::optional<IRArcOperationKind> arc_operation;
    std::vector<MemoryExplanationReason> reasons;
};

struct MemoryExplanationCandidate {
    IROrigin origin;
    std::vector<MemoryExplanationKind> kinds;
};

struct MemoryExplanationResult {
    std::string module_name;
    std::size_t line{0};
    std::optional<std::size_t> column;
    MemoryExplanationStatus status{MemoryExplanationStatus::NoMemoryDecision};
    std::vector<MemoryExplanation> explanations;
    std::vector<MemoryExplanationCandidate> candidates;
};

class MemoryExplainer {
public:
    [[nodiscard]] MemoryExplanationResult explain(
        const IRProgram& program,
        const MemoryReport& memory,
        std::string module_name,
        std::size_t line,
        std::optional<std::size_t> column = std::nullopt) const;
    [[nodiscard]] std::string format(
        const MemoryExplanationResult& result) const;
};

} // namespace toro
