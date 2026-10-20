#include "toro/StorageDecisionAnalyzer.hpp"

#include <string>
#include <unordered_map>

namespace toro {
namespace {

class Analysis {
public:
    Analysis(IRProgram& program, bool enable_stack)
        : program_(program), enable_stack_(enable_stack)
    {
        for (auto& allocation : program_.escape_allocations) {
            allocations_.emplace(allocation.identity, &allocation);
            allocation.storage = IRStorageClass::Heap;
            allocation.storage_reason = fallback_reason(allocation);
        }
    }

    void run()
    {
        for (auto& declaration : program_.declarations) {
            if (!declaration.body) continue;
            callable_has_managed_reassignment_
                = has_managed_reassignment(*declaration.body);
            analyze_block(*declaration.body);
        }
        for (auto& operation : program_.top_level_operations) {
            mark_heap(operation);
        }
        for (auto& declaration : program_.declarations) {
            if (declaration.body) apply_storage(*declaration.body);
        }
        for (auto& operation : program_.top_level_operations) {
            apply_storage(operation);
        }
        publish_metrics();
    }

private:
    static bool has_weak_constraint(const IRAllocationEscape& allocation)
    {
        for (const auto& cause : allocation.causes) {
            if (cause.reason == IREscapeReason::WeakObservation) return true;
        }
        return false;
    }

    static bool has_managed_reassignment(const IROperation& operation)
    {
        if (operation.kind == IROperationKind::Assign
            && !operation.children.empty()
            && operation.children.back().result_type
            && operation.children.back().result_type->category
                == IRValueCategory::ManagedReference) {
            return true;
        }
        for (const auto& child : operation.children) {
            if (has_managed_reassignment(child)) return true;
        }
        return false;
    }

    std::string fallback_reason(const IRAllocationEscape& allocation) const
    {
        if (!enable_stack_) return "stack promotion disabled";
        if (allocation.state == IREscapeState::Escapes) {
            for (const auto& cause : allocation.causes) {
                if (cause.causes_escape && !cause.uncertain) {
                    return "escapes through "
                        + std::string(ir_escape_reason_name(cause.reason));
                }
            }
            return "allocation escapes";
        }
        if (allocation.state == IREscapeState::Unknown) {
            for (const auto& cause : allocation.causes) {
                if (cause.uncertain) {
                    return "escape is uncertain at "
                        + std::string(ir_escape_reason_name(cause.reason));
                }
            }
            return "escape behavior is unknown";
        }
        if (has_weak_constraint(allocation)) {
            return "weak target requires heap control-block lifetime";
        }
        if (callable_has_managed_reassignment_) {
            return "managed reassignment prevents a stable local owner";
        }
        return "construction has no direct local lifetime owner";
    }

    void mark_heap(IROperation& operation)
    {
        if (operation.kind == IROperationKind::Construction
            && !operation.allocation_identity.empty()) {
            operation.storage = IRStorageClass::Heap;
        }
        for (auto& child : operation.children) mark_heap(child);
    }

    void apply_storage(IROperation& operation)
    {
        if (!operation.allocation_identity.empty()) {
            if (const auto found = allocations_.find(
                    operation.allocation_identity);
                found != allocations_.end()) {
                operation.storage = found->second->storage;
            }
        }
        for (auto& child : operation.children) apply_storage(child);
    }

    void analyze_block(IROperation& block)
    {
        for (auto& operation : block.children) analyze_statement(operation);
    }

    void analyze_statement(IROperation& operation)
    {
        if (operation.kind == IROperationKind::Declare
            && operation.children.size() == 1) {
            auto& initializer = operation.children.front();
            if (initializer.kind == IROperationKind::Construction
                && !initializer.allocation_identity.empty()) {
                decide_local(initializer);
            } else {
                mark_heap(initializer);
            }
        } else if (operation.kind == IROperationKind::Block) {
            analyze_block(operation);
            return;
        } else if (operation.kind == IROperationKind::Branch
            || operation.kind == IROperationKind::Loop
            || operation.kind == IROperationKind::ForEach
            || operation.kind == IROperationKind::Handle) {
            for (auto& child : operation.children) {
                if (child.kind == IROperationKind::Block) analyze_block(child);
                else mark_heap(child);
            }
            return;
        } else {
            mark_heap(operation);
            return;
        }
        for (auto& child : operation.children) {
            if (&child != &operation.children.front()) mark_heap(child);
        }
    }

    void decide_local(IROperation& construction)
    {
        const auto found = allocations_.find(construction.allocation_identity);
        if (found == allocations_.end()) {
            construction.storage = IRStorageClass::Heap;
            return;
        }
        auto& allocation = *found->second;
        const bool eligible = enable_stack_
            && allocation.state == IREscapeState::DoesNotEscape
            && !has_weak_constraint(allocation)
            && !callable_has_managed_reassignment_;
        allocation.storage = eligible
            ? IRStorageClass::Stack : IRStorageClass::Heap;
        allocation.storage_reason = eligible
            ? "proven local lifetime" : fallback_reason(allocation);
        construction.storage = allocation.storage;
        for (auto& child : construction.children) mark_heap(child);
    }

    void publish_metrics()
    {
        program_.escape_metrics.stack_allocations = 0;
        program_.escape_metrics.heap_allocations = 0;
        for (const auto& allocation : program_.escape_allocations) {
            if (allocation.storage == IRStorageClass::Stack) {
                ++program_.escape_metrics.stack_allocations;
            } else {
                ++program_.escape_metrics.heap_allocations;
            }
        }
    }

    IRProgram& program_;
    bool enable_stack_;
    bool callable_has_managed_reassignment_{false};
    std::unordered_map<std::string, IRAllocationEscape*> allocations_;
};

} // namespace

void StorageDecisionAnalyzer::analyze(
    IRProgram& program,
    bool enable_stack) const
{
    Analysis(program, enable_stack).run();
}

} // namespace toro
