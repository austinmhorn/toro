#include "toro/StorageDecisionAnalyzer.hpp"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace toro {
namespace {

class Analysis {
public:
    Analysis(
        IRProgram& program,
        bool enable_stack,
        ExperimentalArenaOptions arena)
        : program_(program)
        , enable_stack_(enable_stack)
        , arena_(std::move(arena))
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
        apply_arena_prototype();
        for (auto& declaration : program_.declarations) {
            if (declaration.body) apply_storage(*declaration.body);
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
                operation.region_identity = found->second->region_identity;
            }
        }
        for (auto& child : operation.children) apply_storage(child);
        if (operation.kind == IROperationKind::Declare
            && operation.children.size() == 1
            && operation.children.front().storage) {
            operation.storage = operation.children.front().storage;
            operation.region_identity = operation.children.front().region_identity;
        }
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
        program_.escape_metrics.arena_allocations = 0;
        program_.escape_metrics.heap_allocations = 0;
        program_.escape_metrics.arena_fallbacks = 0;
        for (const auto& allocation : program_.escape_allocations) {
            if (allocation.storage == IRStorageClass::Stack) {
                ++program_.escape_metrics.stack_allocations;
            } else if (allocation.storage == IRStorageClass::Arena) {
                ++program_.escape_metrics.arena_allocations;
            } else {
                ++program_.escape_metrics.heap_allocations;
                if (arena_.enabled
                    && type_name(allocation.type.type) == arena_.type_name) {
                    ++program_.escape_metrics.arena_fallbacks;
                }
            }
        }
        for (auto& region : program_.regions) {
            region.heap_fallbacks = program_.escape_metrics.arena_fallbacks;
        }
    }

    bool declaration_has_weak_field(std::string_view type) const
    {
        for (const auto& declaration : program_.declarations) {
            if (declaration.kind != IRDeclarationKind::Class
                || declaration_source_name(declaration.identity) != type) {
                continue;
            }
            return std::ranges::any_of(declaration.fields,
                [](const auto& field) { return field.is_weak; });
        }
        return true;
    }

    static bool contains_unsafe_exit(const IROperation& operation)
    {
        if (operation.kind == IROperationKind::Return
            || operation.kind == IROperationKind::PropagateResult
            || operation.kind == IROperationKind::Stop
            || operation.kind == IROperationKind::Continue) {
            return true;
        }
        return std::ranges::any_of(operation.children,
            [](const auto& child) { return contains_unsafe_exit(child); });
    }

    bool clears_target_list(const IROperation& operation) const
    {
        if (operation.kind == IROperationKind::CollectionOperation
            && operation.name == "clear" && !operation.children.empty()
            && operation.children.front().result_type) {
            const auto& type = operation.children.front().result_type->type;
            if (type_name(type).starts_with("List<")
                && type.arguments.size() == 1
                && type_name(type.arguments.front()) == arena_.type_name) {
                return true;
            }
        }
        return std::ranges::any_of(operation.children,
            [&](const auto& child) { return clears_target_list(child); });
    }

    void collect_cleared_lists(
        const IROperation& operation,
        std::unordered_set<std::string>& result) const
    {
        if (operation.kind == IROperationKind::CollectionOperation
            && operation.name == "clear" && !operation.children.empty()
            && operation.children.front().kind == IROperationKind::Load
            && operation.children.front().result_type) {
            const auto& type = operation.children.front().result_type->type;
            if (type_name(type).starts_with("List<")
                && type.arguments.size() == 1
                && type_name(type.arguments.front()) == arena_.type_name) {
                result.insert(operation.children.front().name);
            }
        }
        for (const auto& child : operation.children) {
            collect_cleared_lists(child, result);
        }
    }

    static void collect_candidate_bindings(
        const IROperation& operation,
        std::unordered_map<std::string, std::string>& result)
    {
        if (operation.kind == IROperationKind::Declare
            && !operation.children.empty()
            && operation.children.front().kind == IROperationKind::Construction
            && !operation.children.front().allocation_identity.empty()) {
            result.insert_or_assign(
                operation.name, operation.children.front().allocation_identity);
        }
        for (const auto& child : operation.children) {
            collect_candidate_bindings(child, result);
        }
    }

    static void collect_collection_destinations(
        const IROperation& operation,
        const std::unordered_map<std::string, std::string>& bindings,
        std::unordered_map<std::string, std::unordered_set<std::string>>& result)
    {
        if (operation.kind == IROperationKind::CollectionOperation
            && (operation.name == "add" || operation.name == "append")
            && operation.children.size() == 2
            && operation.children.front().kind == IROperationKind::Load
            && operation.children.back().kind == IROperationKind::Load) {
            const auto binding = bindings.find(operation.children.back().name);
            if (binding != bindings.end()) {
                result[binding->second].insert(operation.children.front().name);
            }
        }
        for (const auto& child : operation.children) {
            collect_collection_destinations(child, bindings, result);
        }
    }

    static void mark_region_collection_operations(
        IROperation& operation,
        const std::unordered_map<std::string, std::string>& bindings,
        const std::unordered_set<std::string>& eligible_allocations,
        const std::unordered_set<std::string>& cleared_lists,
        std::string_view region_identity,
        std::size_t& arc_suppressions)
    {
        if (operation.kind == IROperationKind::CollectionOperation
            && operation.name == "clear" && !operation.children.empty()
            && operation.children.front().kind == IROperationKind::Load
            && cleared_lists.contains(operation.children.front().name)) {
            operation.storage = IRStorageClass::Arena;
            operation.region_identity = region_identity;
        } else if (operation.kind == IROperationKind::CollectionOperation
            && (operation.name == "add" || operation.name == "append")
            && operation.children.size() == 2
            && operation.children.front().kind == IROperationKind::Load
            && cleared_lists.contains(operation.children.front().name)
            && operation.children.back().kind == IROperationKind::Load) {
            const auto binding = bindings.find(operation.children.back().name);
            if (binding != bindings.end()
                && eligible_allocations.contains(binding->second)) {
                operation.storage = IRStorageClass::Arena;
                operation.region_identity = region_identity;
                arc_suppressions += 2;
            }
        }
        for (auto& child : operation.children) {
            mark_region_collection_operations(child, bindings,
                eligible_allocations, cleared_lists, region_identity,
                arc_suppressions);
        }
    }

    void collect_target_constructions(
        IROperation& operation,
        std::vector<IROperation*>& result)
    {
        if (operation.kind == IROperationKind::Construction
            && operation.result_type
            && type_name(operation.result_type->type) == arena_.type_name
            && !operation.allocation_identity.empty()) {
            result.push_back(&operation);
        }
        for (auto& child : operation.children) {
            collect_target_constructions(child, result);
        }
    }

    bool arena_eligible(const IRAllocationEscape& allocation) const
    {
        if (allocation.storage == IRStorageClass::Stack
            || has_weak_constraint(allocation)
            || declaration_has_weak_field(arena_.type_name)) {
            return false;
        }
        bool collection_storage = false;
        for (const auto& cause : allocation.causes) {
            if (!cause.causes_escape) continue;
            if (cause.reason != IREscapeReason::CollectionStorage
                || cause.uncertain) {
                return false;
            }
            collection_storage = true;
        }
        return collection_storage;
    }

    void consider_loop(IROperation& operation)
    {
        if (operation.kind == IROperationKind::Loop
            && operation.children.size() >= 2) {
            auto& body = operation.children[1];
            if (clears_target_list(body) && !contains_unsafe_exit(body)) {
                std::unordered_set<std::string> cleared_lists;
                collect_cleared_lists(body, cleared_lists);
                std::unordered_map<std::string, std::string> bindings;
                collect_candidate_bindings(body, bindings);
                std::unordered_map<std::string,
                    std::unordered_set<std::string>> destinations;
                collect_collection_destinations(body, bindings, destinations);
                std::vector<IROperation*> candidates;
                collect_target_constructions(body, candidates);
                std::vector<IROperation*> eligible;
                for (auto* candidate : candidates) {
                    const auto found = allocations_.find(
                        candidate->allocation_identity);
                    const auto stored = destinations.find(
                        candidate->allocation_identity);
                    const bool proven_cleared = stored != destinations.end()
                        && !stored->second.empty()
                        && std::ranges::all_of(stored->second,
                            [&](const auto& list) {
                                return cleared_lists.contains(list);
                            });
                    if (found != allocations_.end()
                        && proven_cleared
                        && arena_eligible(*found->second)) {
                        eligible.push_back(candidate);
                    }
                }
                if (!eligible.empty()) {
                    const std::string identity = "experimental." + arena_.region_name
                        + '@' + operation.origin.module_name + ':'
                        + std::to_string(operation.origin.location.line) + ':'
                        + std::to_string(operation.origin.location.column);
                    body.region_identity = identity;
                    body.region_capacity_bytes = arena_.capacity_bytes;
                    body.region_max_objects = arena_.max_objects;
                    IRRegionInfo region{
                        identity,
                        operation.origin,
                        arena_.capacity_bytes,
                        arena_.max_objects,
                        {},
                        0,
                        0,
                        0,
                        0,
                    };
                    std::unordered_set<std::string> eligible_allocations;
                    for (auto* candidate : eligible) {
                        auto& allocation = *allocations_.at(
                            candidate->allocation_identity);
                        allocation.storage = IRStorageClass::Arena;
                        allocation.storage_reason =
                            "experimental region-bounded collection lifetime";
                        allocation.region_identity = identity;
                        candidate->storage = IRStorageClass::Arena;
                        candidate->region_identity = identity;
                        region.allocation_identities.push_back(
                            candidate->allocation_identity);
                        eligible_allocations.insert(candidate->allocation_identity);
                    }
                    std::size_t arc_suppressions = 0;
                    mark_region_collection_operations(body, bindings,
                        eligible_allocations, cleared_lists, identity,
                        arc_suppressions);
                    region.arena_allocation_sites = eligible.size();
                    region.teardown_entries = eligible.size();
                    region.runtime_arc_suppressions = arc_suppressions;
                    program_.regions.push_back(std::move(region));
                }
            }
        }
        for (auto& child : operation.children) consider_loop(child);
    }

    void apply_arena_prototype()
    {
        program_.regions.clear();
        if (!arena_.enabled || arena_.type_name.empty()
            || arena_.capacity_bytes == 0 || arena_.max_objects == 0) {
            return;
        }
        for (auto& declaration : program_.declarations) {
            if (declaration.body) consider_loop(*declaration.body);
        }
    }

    IRProgram& program_;
    bool enable_stack_;
    ExperimentalArenaOptions arena_;
    bool callable_has_managed_reassignment_{false};
    std::unordered_map<std::string, IRAllocationEscape*> allocations_;
};

} // namespace

void StorageDecisionAnalyzer::analyze(
    IRProgram& program,
    bool enable_stack,
    ExperimentalArenaOptions arena) const
{
    Analysis(program, enable_stack, std::move(arena)).run();
}

} // namespace toro
