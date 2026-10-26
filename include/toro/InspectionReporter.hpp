#pragma once

#include "toro/IR.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace toro {

struct IRInspectionAllocation {
    std::string identity;
    std::string callable_identity;
    IRType type;
    IROrigin origin;
    IREscapeState escape_state{IREscapeState::Unknown};
    IRStorageClass storage{IRStorageClass::Heap};
    std::string reason;
};

struct IRInspectionDispatch {
    std::string callable_identity;
    IROrigin origin;
    IROperationKind operation_kind{IROperationKind::VirtualCall};
    std::string operation_name;
    std::vector<std::string> target_identities;
};

struct IRInspectionArc {
    std::string callable_identity;
    std::size_t retains_before{0};
    std::size_t retains_after{0};
    std::size_t releases_before{0};
    std::size_t releases_after{0};
    std::size_t elided_operations{0};
    std::size_t ownership_transfers{0};
    std::size_t inferred_borrows{0};
};

struct IRInspectionEffect {
    std::string callable_identity;
    IREffect effect{IREffect::Block};
    IROrigin origin;
    std::string operation_name;
};

struct IRInspectionReport {
    std::string entry_module;
    std::vector<IRInspectionAllocation> allocations;
    std::vector<IRInspectionDispatch> dispatches;
    std::vector<IRInspectionArc> arc;
    std::vector<IRInspectionEffect> effects;
    std::size_t capture_count{0};
};

class InspectionReporter {
public:
    [[nodiscard]] IRInspectionReport inspect(const IRProgram& program) const;
    [[nodiscard]] std::string format(const IRInspectionReport& report) const;
    [[nodiscard]] std::string report(const IRProgram& program) const;
};

} // namespace toro
