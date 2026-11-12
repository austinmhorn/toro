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
    Analysis(IRProgram& program, bool enable_stack)
        : program_(program)
        , enable_stack_(enable_stack)
    {
        for (auto& allocation : program_.escape_allocations) {
            allocations_.emplace(allocation.identity, &allocation);
            allocation.storage = IRStorageClass::Heap;
            allocation.storage_reason = fallback_reason(allocation);
        }
        for (const auto& declaration : program_.declarations) {
            for (const auto& field : declaration.fields) {
                if (field.is_weak) weak_field_names_.insert(field.name);
            }
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
        apply_source_arenas();
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
        } else if (operation.kind == IROperationKind::Region
            && operation.children.size() == 1
            && operation.children.front().kind == IROperationKind::Block) {
            analyze_block(operation.children.front());
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
                if (arena_candidates_.contains(allocation.identity)) {
                    ++program_.escape_metrics.arena_fallbacks;
                }
            }
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

    void collect_cleared_lists(
        const IROperation& operation,
        std::unordered_set<std::string>& result) const
    {
        if (operation.kind == IROperationKind::Region) return;
        if (operation.kind == IROperationKind::CollectionOperation
            && operation.name == "clear" && !operation.children.empty()
            && operation.children.front().kind == IROperationKind::Load
            && operation.children.front().result_type) {
            const auto& type = operation.children.front().result_type->type;
            if (type_name(type).starts_with("List<")
                && type.arguments.size() == 1) {
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
        if (operation.kind == IROperationKind::Region) return;
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
        if (operation.kind == IROperationKind::Region) return;
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

    static void add_dependency(
        std::string_view value,
        std::string_view owner,
        std::unordered_map<std::string, std::unordered_set<std::string>>& result)
    {
        result[std::string(value)].insert(std::string(owner));
    }

    void collect_strong_dependencies(
        const IROperation& operation,
        const std::unordered_map<std::string, std::string>& bindings,
        std::unordered_map<std::string, std::unordered_set<std::string>>& result) const
    {
        if (operation.kind == IROperationKind::Region) return;
        if (operation.kind == IROperationKind::Construction) {
            for (const auto& child : operation.children) {
                if (child.kind != IROperationKind::Load) continue;
                const auto binding = bindings.find(child.name);
                if (binding != bindings.end()) {
                    add_dependency(binding->second,
                        operation.allocation_identity, result);
                }
            }
        } else if (operation.kind == IROperationKind::MemberAssign
            && !weak_field_names_.contains(operation.name)
            && operation.children.size() == 2
            && operation.children.back().kind == IROperationKind::Load) {
            const auto value = bindings.find(operation.children.back().name);
            if (value != bindings.end()) {
                std::string owner = "<longer-lived>";
                if (operation.children.front().kind == IROperationKind::Load) {
                    const auto target = bindings.find(
                        operation.children.front().name);
                    if (target != bindings.end()) owner = target->second;
                }
                add_dependency(value->second, owner, result);
            }
        }
        for (const auto& child : operation.children) {
            collect_strong_dependencies(child, bindings, result);
        }
    }

    static void collect_cloned_collections(
        const IROperation& operation,
        std::unordered_set<std::string>& result)
    {
        if (operation.kind == IROperationKind::Region) return;
        if (operation.kind == IROperationKind::CollectionOperation
            && operation.name == "clone" && !operation.children.empty()
            && operation.children.front().kind == IROperationKind::Load) {
            result.insert(operation.children.front().name);
        }
        for (const auto& child : operation.children) {
            collect_cloned_collections(child, result);
        }
    }

    static void collect_reassigned_bindings(
        const IROperation& operation,
        const std::unordered_map<std::string, std::string>& bindings,
        std::unordered_set<std::string>& result)
    {
        if (operation.kind == IROperationKind::Region) return;
        if (operation.kind == IROperationKind::Assign) {
            const auto binding = bindings.find(operation.name);
            if (binding != bindings.end()) result.insert(binding->second);
        }
        for (const auto& child : operation.children) {
            collect_reassigned_bindings(child, bindings, result);
        }
    }

    void collect_weak_observations(
        const IROperation& operation,
        const std::unordered_map<std::string, std::string>& bindings,
        std::unordered_set<std::string>& result) const
    {
        if (operation.kind == IROperationKind::Region) return;
        if (operation.kind == IROperationKind::MemberAssign
            && weak_field_names_.contains(operation.name)
            && !operation.children.empty()
            && operation.children.back().kind == IROperationKind::Load) {
            const auto binding = bindings.find(operation.children.back().name);
            if (binding != bindings.end()) result.insert(binding->second);
        }
        for (const auto& child : operation.children) {
            collect_weak_observations(child, bindings, result);
        }
    }

    static void mark_region_collection_operations(
        IROperation& operation,
        const std::unordered_map<std::string, std::string>& bindings,
        const std::unordered_set<std::string>& eligible_allocations,
        const std::unordered_set<std::string>& region_lists,
        std::string_view region_identity,
        std::size_t& arc_suppressions)
    {
        if (operation.kind == IROperationKind::Region) return;
        if (operation.kind == IROperationKind::CollectionOperation
            && operation.name == "clear" && !operation.children.empty()
            && operation.children.front().kind == IROperationKind::Load
            && region_lists.contains(operation.children.front().name)) {
            operation.storage = IRStorageClass::Arena;
            operation.region_identity = region_identity;
        } else if (operation.kind == IROperationKind::CollectionOperation
            && (operation.name == "add" || operation.name == "append")
            && operation.children.size() == 2
            && operation.children.front().kind == IROperationKind::Load
            && region_lists.contains(operation.children.front().name)
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
                eligible_allocations, region_lists, region_identity,
                arc_suppressions);
        }
    }

    static void collect_region_constructions(
        IROperation& operation,
        std::vector<IROperation*>& result)
    {
        if (operation.kind == IROperationKind::Region) return;
        if (operation.kind == IROperationKind::Construction
            && operation.result_type
            && operation.result_type->category == IRValueCategory::ManagedReference
            && !operation.allocation_identity.empty()) {
            result.push_back(&operation);
        }
        for (auto& child : operation.children) {
            collect_region_constructions(child, result);
        }
    }

    bool source_arena_eligible(
        const IRAllocationEscape& allocation,
        bool stored_in_cleared_collection) const
    {
        if ((allocation.storage == IRStorageClass::Stack
                && !stored_in_cleared_collection)
            || has_weak_constraint(allocation)
            || declaration_has_weak_field(
                declaration_source_name(allocation.type.type.name))) {
            return false;
        }
        if (allocation.state == IREscapeState::DoesNotEscape) return true;
        bool compatible_storage = false;
        for (const auto& cause : allocation.causes) {
            if (!cause.causes_escape) continue;
            const bool region_bounded_cause = stored_in_cleared_collection
                && (cause.reason == IREscapeReason::CollectionStorage
                    || cause.reason == IREscapeReason::StrongField
                    || cause.reason == IREscapeReason::CallArgument);
            if (!region_bounded_cause) {
                return false;
            }
            compatible_storage = true;
        }
        return compatible_storage;
    }

    void consider_source_region(
        IROperation& operation,
        std::string_view parent_region)
    {
        if (operation.kind == IROperationKind::Region) {
            operation.parent_region_identity = parent_region;
            if (operation.children.size() != 1
                || operation.children.front().kind != IROperationKind::Block) {
                return;
            }
            auto& body = operation.children.front();
            const std::string identity = operation.region_identity;
            body.region_identity = identity;
            body.parent_region_identity = std::string(parent_region);
            IRRegionInfo region;
            region.identity = identity;
            region.parent_identity = std::string(parent_region);
            region.origin = operation.origin;

            std::vector<IROperation*> candidates;
            collect_region_constructions(body, candidates);
            std::unordered_map<std::string, std::string> bindings;
            collect_candidate_bindings(body, bindings);
            std::unordered_set<std::string> cleared_lists;
            collect_cleared_lists(body, cleared_lists);
            std::unordered_map<std::string,
                std::unordered_set<std::string>> destinations;
            collect_collection_destinations(body, bindings, destinations);
            std::unordered_map<std::string,
                std::unordered_set<std::string>> strong_dependencies;
            collect_strong_dependencies(body, bindings, strong_dependencies);
            std::unordered_set<std::string> weak_observations;
            collect_weak_observations(body, bindings, weak_observations);
            std::unordered_set<std::string> cloned_collections;
            collect_cloned_collections(body, cloned_collections);
            std::unordered_set<std::string> reassigned_bindings;
            collect_reassigned_bindings(body, bindings, reassigned_bindings);
            std::unordered_set<std::string> candidate_identities;
            for (const auto* candidate : candidates) {
                candidate_identities.insert(candidate->allocation_identity);
            }
            const auto compatible_region_owner = [&](const std::string& owner) {
                if (!candidate_identities.contains(owner)
                    || weak_observations.contains(owner)
                    || reassigned_bindings.contains(owner)) {
                    return false;
                }
                const auto allocation = allocations_.find(owner);
                if (allocation == allocations_.end()
                    || !source_arena_eligible(*allocation->second, true)) {
                    return false;
                }
                const auto owner_destinations = destinations.find(owner);
                if (owner_destinations == destinations.end()) return true;
                return !contains_unsafe_exit(body)
                    && std::ranges::all_of(owner_destinations->second,
                        [&](const auto& list) {
                            return cleared_lists.contains(list)
                                && !cloned_collections.contains(list);
                        });
            };
            std::unordered_set<std::string> eligible_allocations;
            for (auto* candidate : candidates) {
                arena_candidates_.insert(candidate->allocation_identity);
                const auto found = allocations_.find(candidate->allocation_identity);
                if (found != allocations_.end()) {
                    found->second->region_identity = identity;
                    candidate->region_identity = identity;
                }
                const auto stored = destinations.find(
                    candidate->allocation_identity);
                const bool stored_in_collection = stored != destinations.end()
                    && !stored->second.empty();
                const bool collection_was_cloned = stored_in_collection
                    && std::ranges::any_of(stored->second,
                        [&](const auto& list) {
                            return cloned_collections.contains(list);
                        });
                const auto owners = strong_dependencies.find(
                    candidate->allocation_identity);
                const bool stored_in_region_graph =
                    owners != strong_dependencies.end()
                    && !owners->second.empty()
                    && std::ranges::all_of(owners->second,
                        compatible_region_owner);
                const bool stored_in_cleared_collection =
                    stored_in_region_graph || (stored_in_collection
                    && std::ranges::all_of(stored->second,
                        [&](const auto& list) {
                            return cleared_lists.contains(list);
                        }));
                const bool unsafe_collection_exit = stored_in_collection
                    && contains_unsafe_exit(body);
                if ((stored_in_collection && !stored_in_cleared_collection)
                    || collection_was_cloned || unsafe_collection_exit) {
                    if (found != allocations_.end()) {
                        found->second->storage = IRStorageClass::Heap;
                        found->second->storage_reason =
                            collection_was_cloned
                            ? "arena candidate escapes through a cloned collection"
                            : unsafe_collection_exit
                            ? "arena collection lifetime is not proven across an early exit"
                            : "arena candidate escapes through an uncleared collection";
                    }
                    continue;
                }
                if (reassigned_bindings.contains(candidate->allocation_identity)) {
                    if (found != allocations_.end()) {
                        found->second->storage = IRStorageClass::Heap;
                        found->second->storage_reason =
                            "arena candidate has unsupported early logical death";
                    }
                    continue;
                }
                if (owners != strong_dependencies.end()
                    && !stored_in_region_graph) {
                    if (found != allocations_.end()) {
                        found->second->storage = IRStorageClass::Heap;
                        found->second->storage_reason =
                            "arena candidate is retained by a longer-lived strong field";
                    }
                    continue;
                }
                if (weak_observations.contains(candidate->allocation_identity)) {
                    if (found != allocations_.end()) {
                        found->second->storage = IRStorageClass::Heap;
                        found->second->storage_reason =
                            "weak target requires heap control-block lifetime";
                    }
                    continue;
                }
                if (found == allocations_.end()) {
                    continue;
                }
                if (!source_arena_eligible(
                        *found->second, stored_in_cleared_collection)) {
                    if (found->second->storage == IRStorageClass::Stack
                        && stored_in_cleared_collection) {
                        found->second->storage = IRStorageClass::Heap;
                        found->second->storage_reason = fallback_reason(
                            *found->second);
                    }
                    continue;
                }
                auto& allocation = *found->second;
                allocation.storage = IRStorageClass::Arena;
                allocation.storage_reason =
                    "construction is bounded by source arena lifetime";
                allocation.region_identity = identity;
                candidate->storage = IRStorageClass::Arena;
                candidate->region_identity = identity;
                region.allocation_identities.push_back(
                    candidate->allocation_identity);
                eligible_allocations.insert(candidate->allocation_identity);
            }

            bool changed = true;
            while (changed) {
                changed = false;
                for (auto* candidate : candidates) {
                    const std::string& candidate_identity =
                        candidate->allocation_identity;
                    if (!eligible_allocations.contains(candidate_identity)) {
                        continue;
                    }
                    bool incompatible = false;
                    if (const auto owners = strong_dependencies.find(
                            candidate_identity);
                        owners != strong_dependencies.end()) {
                        incompatible = std::ranges::any_of(owners->second,
                            [&](const auto& owner) {
                                return candidate_identities.contains(owner)
                                    && !eligible_allocations.contains(owner);
                            });
                    }
                    if (const auto lists = destinations.find(candidate_identity);
                        !incompatible && lists != destinations.end()) {
                        incompatible = std::ranges::any_of(lists->second,
                            [&](const auto& list) {
                                return std::ranges::any_of(destinations,
                                    [&](const auto& entry) {
                                        return entry.second.contains(list)
                                            && !eligible_allocations.contains(
                                                entry.first);
                                    });
                            });
                    }
                    if (!incompatible) continue;
                    eligible_allocations.erase(candidate_identity);
                    region.allocation_identities.erase(std::remove(
                        region.allocation_identities.begin(),
                        region.allocation_identities.end(), candidate_identity),
                        region.allocation_identities.end());
                    candidate->storage = IRStorageClass::Heap;
                    if (const auto allocation = allocations_.find(candidate_identity);
                        allocation != allocations_.end()) {
                        allocation->second->storage = IRStorageClass::Heap;
                        allocation->second->storage_reason =
                            "arena ownership graph includes heap-backed storage";
                    }
                    changed = true;
                }
            }

            std::unordered_set<std::string> region_lists;
            for (const auto& list : cleared_lists) {
                std::vector<std::string> stored_allocations;
                for (const auto& [allocation, lists] : destinations) {
                    if (lists.contains(list)) stored_allocations.push_back(allocation);
                }
                if (!stored_allocations.empty()
                    && std::ranges::all_of(stored_allocations,
                        [&](const auto& allocation) {
                            return eligible_allocations.contains(allocation);
                        })) {
                    region_lists.insert(list);
                }
            }
            std::size_t arc_suppressions = 0;
            mark_region_collection_operations(body, bindings,
                eligible_allocations, region_lists, identity,
                arc_suppressions);
            region.arena_allocation_sites = eligible_allocations.size();
            region.teardown_entries = eligible_allocations.size();
            region.teardown_order.assign(
                region.allocation_identities.rbegin(),
                region.allocation_identities.rend());
            region.runtime_arc_suppressions = arc_suppressions;
            region.heap_fallbacks = candidates.size() - eligible_allocations.size();
            program_.regions.push_back(std::move(region));

            for (auto& child : body.children) {
                consider_source_region(child, identity);
            }
            return;
        }
        for (auto& child : operation.children) {
            consider_source_region(child, parent_region);
        }
    }

    void apply_source_arenas()
    {
        program_.regions.clear();
        arena_candidates_.clear();
        for (auto& declaration : program_.declarations) {
            if (declaration.body) consider_source_region(*declaration.body, {});
        }
        for (auto& operation : program_.top_level_operations) {
            consider_source_region(operation, {});
        }
    }

    IRProgram& program_;
    bool enable_stack_;
    bool callable_has_managed_reassignment_{false};
    std::unordered_map<std::string, IRAllocationEscape*> allocations_;
    std::unordered_set<std::string> arena_candidates_;
    std::unordered_set<std::string> weak_field_names_;
};

} // namespace

void StorageDecisionAnalyzer::analyze(
    IRProgram& program,
    bool enable_stack) const
{
    Analysis(program, enable_stack).run();
}

} // namespace toro
