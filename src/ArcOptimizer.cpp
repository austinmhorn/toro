#include "toro/ArcOptimizer.hpp"

#include "toro/OwnershipSummaryAnalyzer.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>

namespace toro {
namespace {

bool is_shared_reference(const IRType& type)
{
    if (type.category == IRValueCategory::ManagedReference
        || type.category == IRValueCategory::InterfaceValue) {
        return true;
    }
    return type.category == IRValueCategory::CollectionValue
        && (type.type.name == "List" || type.type.name == "Map");
}

bool owns_runtime_storage(const IRType& type)
{
    return is_shared_reference(type)
        || type.category == IRValueCategory::CollectionValue;
}

bool is_callable(const IRDeclaration& declaration)
{
    return declaration.kind == IRDeclarationKind::Function
        || declaration.kind == IRDeclarationKind::ForeignFunction
        || declaration.kind == IRDeclarationKind::Method
        || declaration.kind == IRDeclarationKind::Conversion;
}

void append_operation(
    IRCallableArcSchedule& schedule,
    IRArcOperationKind kind,
    std::string value,
    const IROrigin& origin,
    std::string reason,
    std::size_t pair_id = 0)
{
    schedule.unoptimized_operations.push_back(IRArcOperation{
        kind,
        std::move(value),
        origin,
        std::move(reason),
        pair_id,
        false,
    });
}

bool expression_is_borrowed(const IROperation& operation)
{
    return operation.kind == IROperationKind::Load
        || operation.kind == IROperationKind::MemberAccess
        || operation.kind == IROperationKind::IndexAccess;
}

void materialize_operation(
    const IROperation& operation,
    IRCallableArcSchedule& schedule,
    std::size_t& next_pair)
{
    switch (operation.kind) {
    case IROperationKind::Construction:
        if (operation.result_type && is_shared_reference(*operation.result_type)) {
            if (operation.storage != IRStorageClass::Stack) {
                append_operation(schedule, IRArcOperationKind::OwnedTemporary,
                    operation.name, operation.origin, "class construction result");
            }
        }
        break;
    case IROperationKind::Declare:
    case IROperationKind::Assign: {
        const IRType* assigned_type = operation.result_type
            ? &*operation.result_type
            : (!operation.children.empty() && operation.children.back().result_type
                    ? &*operation.children.back().result_type
                    : nullptr);
        if (assigned_type && owns_runtime_storage(*assigned_type)
            && operation.storage != IRStorageClass::Stack) {
            append_operation(schedule, IRArcOperationKind::ManagedAssignment,
                operation.name, operation.origin,
                operation.kind == IROperationKind::Declare
                    ? "managed local initialization"
                    : "managed local replacement");
            const auto pair = next_pair++;
            if (is_shared_reference(*assigned_type)
                && !operation.children.empty()
                && expression_is_borrowed(operation.children.back())) {
                append_operation(schedule, IRArcOperationKind::Retain,
                    operation.name, operation.origin,
                    "borrowed value stored in owning local", pair);
            }
            append_operation(schedule, IRArcOperationKind::Release,
                operation.name, operation.origin,
                operation.kind == IROperationKind::Declare
                    ? "owning local scope cleanup"
                    : "replaced managed local value",
                pair);
        }
        break;
    }
    case IROperationKind::MemberAssign:
    case IROperationKind::IndexAssign:
        if (!operation.children.empty()) {
            const auto& value = operation.children.back();
            if (value.result_type && is_shared_reference(*value.result_type)) {
                const auto pair = next_pair++;
                append_operation(schedule, IRArcOperationKind::ManagedAssignment,
                    operation.name, operation.origin,
                    operation.kind == IROperationKind::MemberAssign
                        ? "strong field replacement"
                        : "managed collection element replacement");
                append_operation(schedule, IRArcOperationKind::Retain,
                    operation.name, operation.origin,
                    "new stored reference", pair);
                append_operation(schedule, IRArcOperationKind::Release,
                    operation.name, operation.origin,
                    "replaced stored reference", pair);
            }
        }
        break;
    case IROperationKind::Return:
        if (!operation.children.empty()
            && operation.children.front().result_type
            && is_shared_reference(*operation.children.front().result_type)
            && expression_is_borrowed(operation.children.front())) {
            append_operation(schedule, IRArcOperationKind::Retain,
                "return", operation.origin,
                "borrowed reference transferred to caller", next_pair++);
        }
        break;
    case IROperationKind::Call:
    case IROperationKind::ForeignCall:
    case IROperationKind::VirtualCall:
    case IROperationKind::InterfaceCall:
        break;
    case IROperationKind::CollectionOperation:
        if (operation.name == "Array" || operation.name == "List"
            || operation.name == "Map" || operation.name == "add"
            || operation.name == "append" || operation.name == "insert") {
            const std::size_t start = operation.name == "Array"
                    || operation.name == "List" || operation.name == "Map"
                ? 0 : 1;
            for (std::size_t index = start;
                 index < operation.children.size(); ++index) {
                const auto& child = operation.children[index];
                if (!child.result_type || !is_shared_reference(*child.result_type)) {
                    continue;
                }
                const auto pair = next_pair++;
                append_operation(schedule, IRArcOperationKind::Retain,
                    child.name.empty() ? "collection element" : child.name,
                    operation.origin, "reference stored in collection", pair);
                append_operation(schedule, IRArcOperationKind::Release,
                    child.name.empty() ? "collection element" : child.name,
                    operation.origin, "collection element cleanup", pair);
            }
        }
        break;
    case IROperationKind::EnumConstruction:
    case IROperationKind::ResultConstruction:
        for (const auto& child : operation.children) {
            if (!child.result_type || !is_shared_reference(*child.result_type)) {
                continue;
            }
            const auto pair = next_pair++;
            append_operation(schedule, IRArcOperationKind::Retain,
                child.name.empty() ? "payload" : child.name,
                operation.origin, "managed payload ownership", pair);
            append_operation(schedule, IRArcOperationKind::Release,
                child.name.empty() ? "payload" : child.name,
                operation.origin, "managed payload cleanup", pair);
        }
        break;
    case IROperationKind::Block:
    case IROperationKind::Constant:
    case IROperationKind::Load:
    case IROperationKind::Unary:
    case IROperationKind::Binary:
    case IROperationKind::MemberAccess:
    case IROperationKind::IndexAccess:
    case IROperationKind::Conversion:
    case IROperationKind::PropagateResult:
    case IROperationKind::Branch:
    case IROperationKind::Loop:
    case IROperationKind::ForEach:
    case IROperationKind::Handle:
    case IROperationKind::Stop:
    case IROperationKind::Continue:
        break;
    }

    for (const auto& child : operation.children) {
        materialize_operation(child, schedule, next_pair);
    }
}

std::size_t load_count(const IROperation& operation, std::string_view name)
{
    std::size_t result = operation.kind == IROperationKind::Load
            && operation.name == name
        ? 1 : 0;
    for (const auto& child : operation.children) {
        result += load_count(child, name);
    }
    return result;
}

bool used_after(
    const IROperation& block,
    std::size_t statement_index,
    std::string_view name)
{
    for (std::size_t index = statement_index + 1;
         index < block.children.size(); ++index) {
        if (load_count(block.children[index], name) != 0) return true;
    }
    return false;
}

const IROperation* direct_load(const IROperation& operation)
{
    return operation.kind == IROperationKind::Load ? &operation : nullptr;
}

class TransferAnalysis {
public:
    explicit TransferAnalysis(IRProgram& program) : program_(program)
    {
        for (const auto& declaration : program.declarations) {
            declarations_.emplace(declaration.identity, &declaration);
        }
    }

    void run()
    {
        for (auto& declaration : program_.declarations) {
            if (!declaration.body) continue;
            std::unordered_map<std::string, IROrigin> owned;
            for (const auto& parameter : declaration.parameters) {
                if (is_shared_reference(parameter.type)) {
                    owned.emplace(parameter.name, parameter.origin);
                }
            }
            analyze_block(*declaration.body, std::move(owned));
        }
        for (auto& operation : program_.top_level_operations) {
            if (operation.kind == IROperationKind::Block) {
                analyze_block(operation, {});
            }
        }
    }

private:
    bool strong_field(const IROperation& operation) const
    {
        if (operation.kind != IROperationKind::MemberAssign
            || operation.children.empty()
            || !operation.children.front().result_type) {
            return false;
        }
        std::string owner = operation.children.front().result_type->type.name;
        while (!owner.empty()) {
            const auto found = declarations_.find(owner);
            if (found == declarations_.end()) return false;
            for (const auto& field : found->second->fields) {
                if (field.name == operation.name) {
                    return !field.is_weak
                        && field.type.category == IRValueCategory::ManagedReference;
                }
            }
            std::string base;
            for (const auto& related : found->second->related_types) {
                if (related.category == IRValueCategory::ManagedReference) {
                    base = related.type.name;
                    break;
                }
            }
            owner = std::move(base);
        }
        return false;
    }

    void mark(
        IROperation& operation,
        const IROperation& source,
        std::string destination,
        std::unordered_map<std::string, IROrigin>& owned)
    {
        if (!owned.contains(source.name) || source.name == destination) return;
        operation.ownership_transfer = IROwnershipTransfer{
            source.name, std::move(destination), owned.at(source.name)};
        owned.erase(source.name);
    }

    void analyze_block(
        IROperation& block,
        std::unordered_map<std::string, IROrigin> owned)
    {
        if (block.kind != IROperationKind::Block) return;
        for (std::size_t index = 0; index < block.children.size(); ++index) {
            auto& operation = block.children[index];
            const IROperation* source = nullptr;
            std::string destination;
            bool eligible = false;
            switch (operation.kind) {
            case IROperationKind::Declare:
                if (!operation.children.empty()) {
                    source = direct_load(operation.children.front());
                    destination = operation.name;
                    eligible = source && operation.result_type
                        && is_shared_reference(*operation.result_type)
                        && operation.storage != IRStorageClass::Stack;
                }
                break;
            case IROperationKind::Assign:
                if (!operation.children.empty()) {
                    source = direct_load(operation.children.front());
                    destination = operation.name;
                    eligible = source && source->result_type
                        && is_shared_reference(*source->result_type)
                        && owned.contains(destination);
                }
                break;
            case IROperationKind::Return:
                if (!operation.children.empty()) {
                    source = direct_load(operation.children.front());
                    destination = "return";
                    eligible = source && source->result_type
                        && is_shared_reference(*source->result_type);
                    if (!source && operation.children.front().kind
                            == IROperationKind::Construction
                        && operation.children.front().result_type
                        && is_shared_reference(
                            *operation.children.front().result_type)) {
                        operation.ownership_transfer = IROwnershipTransfer{
                            "<temporary>", "return",
                            operation.children.front().origin};
                    }
                }
                break;
            case IROperationKind::MemberAssign:
                if (operation.children.size() == 2 && strong_field(operation)) {
                    source = direct_load(operation.children.back());
                    destination = "field " + operation.name;
                    eligible = source && source->result_type
                        && is_shared_reference(*source->result_type);
                }
                break;
            case IROperationKind::IndexAssign:
                if (!operation.children.empty()) {
                    source = direct_load(operation.children.back());
                    destination = "collection element";
                    eligible = source && source->result_type
                        && is_shared_reference(*source->result_type);
                }
                break;
            case IROperationKind::CollectionOperation:
                if ((operation.name == "add" || operation.name == "append")
                    && operation.children.size() == 2) {
                    source = direct_load(operation.children.back());
                    destination = "collection element";
                    eligible = source && source->result_type
                        && is_shared_reference(*source->result_type);
                }
                break;
            default:
                break;
            }
            if (eligible && source && load_count(operation, source->name) == 1
                && !used_after(block, index, source->name)) {
                mark(operation, *source, destination, owned);
            }
            if (operation.kind == IROperationKind::Declare
                && operation.result_type
                && is_shared_reference(*operation.result_type)) {
                owned.insert_or_assign(operation.name, operation.origin);
            }

            // Nested control flow gets independent local reasoning. Ownership
            // from an outer scope is deliberately not transferred there.
            for (auto& child : operation.children) {
                if (child.kind == IROperationKind::Block) {
                    analyze_block(child, {});
                }
            }
        }
    }

    IRProgram& program_;
    std::unordered_map<std::string, const IRDeclaration*> declarations_;
};

void apply_transfer_operations(
    const IROperation& operation,
    IRCallableArcSchedule& schedule,
    std::size_t& transfer_count)
{
    if (operation.ownership_transfer) {
        const auto& transfer = *operation.ownership_transfer;
        const auto& source = transfer.source;
        const auto& destination = transfer.destination;
        if (source != "<temporary>") {
            for (auto& arc : schedule.optimized_operations) {
                if (arc.elided) continue;
                const bool at_destination = arc.origin.module_name
                        == operation.origin.module_name
                    && arc.origin.location.line == operation.origin.location.line
                    && arc.origin.location.column
                        == operation.origin.location.column;
                const bool source_release = arc.kind == IRArcOperationKind::Release
                    && arc.value == source
                    && arc.origin.module_name
                        == transfer.source_origin.module_name
                    && arc.origin.location.line
                        == transfer.source_origin.location.line
                    && arc.origin.location.column
                        == transfer.source_origin.location.column
                    && (arc.reason == "owning local scope cleanup"
                        || arc.reason == "owned parameter exit");
                bool destination_retain = false;
                if (operation.kind == IROperationKind::Return) {
                    destination_retain = arc.kind == IRArcOperationKind::Retain
                        && arc.value == "return";
                } else if (operation.kind == IROperationKind::MemberAssign) {
                    destination_retain = arc.kind == IRArcOperationKind::Retain
                        && arc.value == operation.name
                        && arc.reason == "new stored reference";
                } else if (operation.kind == IROperationKind::IndexAssign) {
                    destination_retain = arc.kind == IRArcOperationKind::Retain
                        && arc.reason == "new stored reference";
                } else if (operation.kind == IROperationKind::CollectionOperation) {
                    destination_retain = arc.kind == IRArcOperationKind::Retain
                        && arc.value == source
                        && arc.reason == "reference stored in collection";
                } else {
                    destination_retain = arc.kind == IRArcOperationKind::Retain
                        && arc.value == operation.name
                        && arc.reason == "borrowed value stored in owning local";
                }
                destination_retain = destination_retain && at_destination;
                if (source_release || destination_retain) arc.elided = true;
            }
        }
        schedule.optimized_operations.push_back(IRArcOperation{
            IRArcOperationKind::OwnershipTransfer,
            source + " -> " + destination,
            operation.origin,
            "final-use ownership transfer",
            0,
            false,
        });
        ++transfer_count;
    }
    for (const auto& child : operation.children) {
        apply_transfer_operations(child, schedule, transfer_count);
    }
}

void elide_pair(
    IRCallableArcSchedule& schedule,
    std::size_t pair,
    std::size_t& elided_pairs)
{
    if (pair == 0) return;
    bool changed = false;
    for (auto& operation : schedule.optimized_operations) {
        if (operation.pair_id == pair && !operation.elided) {
            operation.elided = true;
            changed = true;
        }
    }
    if (changed) ++elided_pairs;
}

std::size_t local_alias_pair(
    const IROperation& operation,
    const IRBorrowUse& borrow,
    const IRCallableArcSchedule& schedule)
{
    for (const auto& arc : schedule.optimized_operations) {
        if (arc.kind == IRArcOperationKind::Retain
            && arc.value == borrow.destination
            && arc.origin.module_name == operation.origin.module_name
            && arc.origin.location.line == operation.origin.location.line
            && arc.origin.location.column == operation.origin.location.column
            && arc.reason == "borrowed value stored in owning local") {
            return arc.pair_id;
        }
    }
    return 0;
}

void apply_borrow_operations(
    const IROperation& operation,
    IRCallableArcSchedule& schedule,
    std::size_t& borrow_count,
    std::size_t& elided_pairs)
{
    for (const auto& borrow : operation.borrows) {
        if (borrow.kind == IRBorrowKind::LocalAlias) {
            elide_pair(schedule,
                local_alias_pair(operation, borrow, schedule), elided_pairs);
        }
        schedule.optimized_operations.push_back(IRArcOperation{
            IRArcOperationKind::BorrowedTemporary,
            borrow.source + " -> " + borrow.destination,
            borrow.origin,
            borrow.reason,
            0,
            false,
        });
        ++borrow_count;
    }
    for (const auto& child : operation.children) {
        apply_borrow_operations(
            child, schedule, borrow_count, elided_pairs);
    }
}

void update_metrics(IRProgram& program)
{
    program.arc_metrics = {};
    for (const auto& schedule : program.arc_schedules) {
        for (const auto& operation : schedule.unoptimized_operations) {
            if (operation.kind == IRArcOperationKind::Retain) {
                ++program.arc_metrics.retains_before;
            } else if (operation.kind == IRArcOperationKind::Release) {
                ++program.arc_metrics.releases_before;
            }
        }
        for (const auto& operation : schedule.optimized_operations) {
            if (operation.elided) continue;
            if (operation.kind == IRArcOperationKind::Retain) {
                ++program.arc_metrics.retains_after;
            } else if (operation.kind == IRArcOperationKind::Release) {
                ++program.arc_metrics.releases_after;
            }
        }
    }
}

} // namespace

void ArcMaterializer::materialize(IRProgram& program) const
{
    program.arc_schedules.clear();
    program.arc_metrics = {};
    std::size_t next_pair = 1;

    for (const auto& declaration : program.declarations) {
        if (is_callable(declaration) && declaration.body) {
            IRCallableArcSchedule schedule;
            schedule.callable_identity = declaration.identity;
            for (const auto& parameter : declaration.parameters) {
                if (!is_shared_reference(parameter.type)) continue;
                const auto pair = next_pair++;
                append_operation(schedule, IRArcOperationKind::Retain,
                    parameter.name, parameter.origin,
                    "owned parameter entry", pair);
                append_operation(schedule, IRArcOperationKind::Release,
                    parameter.name, parameter.origin,
                    "owned parameter exit", pair);
            }
            materialize_operation(*declaration.body, schedule, next_pair);
            schedule.optimized_operations = schedule.unoptimized_operations;
            program.arc_schedules.push_back(std::move(schedule));
        }
        if (declaration.kind == IRDeclarationKind::Class) {
            IRCallableArcSchedule schedule;
            schedule.callable_identity = declaration.identity + "#lifetime";
            append_operation(schedule, IRArcOperationKind::DestroyFinalize,
                declaration.name, declaration.origin,
                "final strong release runs destroy and frees most-derived object");
            for (const auto& field : declaration.fields) {
                if (field.is_weak) {
                    append_operation(schedule, IRArcOperationKind::BorrowedTemporary,
                        field.name, field.origin,
                        "weak slot observes control block without retaining target");
                } else if (is_shared_reference(field.type)) {
                    append_operation(schedule, IRArcOperationKind::Release,
                        field.name, field.origin,
                        "strong field teardown", next_pair++);
                }
            }
            schedule.optimized_operations = schedule.unoptimized_operations;
            program.arc_schedules.push_back(std::move(schedule));
        }
    }

    if (!program.top_level_operations.empty()) {
        IRCallableArcSchedule schedule;
        schedule.callable_identity = "<top-level>";
        for (const auto& operation : program.top_level_operations) {
            materialize_operation(operation, schedule, next_pair);
        }
        schedule.optimized_operations = schedule.unoptimized_operations;
        program.arc_schedules.push_back(std::move(schedule));
    }

    program.lifetime_model = IRLifetimeModel::ExplicitArc;
    program.arc_stage = IRArcStage::Materialized;
    update_metrics(program);
}

void OwnershipTransferAnalyzer::analyze(IRProgram& program) const
{
    if (program.arc_stage != IRArcStage::Materialized) {
        throw std::runtime_error(
            "ownership-transfer analysis requires materialized lifetime IR");
    }
    if (std::ranges::any_of(program.declarations, is_callable)
        && program.ownership_summaries.empty()) {
        throw std::runtime_error(
            "ownership-transfer analysis requires ownership summaries");
    }
    TransferAnalysis(program).run();
    program.arc_stage = IRArcStage::TransfersAnalyzed;
}

void ArcOptimizer::optimize(IRProgram& program) const
{
    if (program.arc_stage != IRArcStage::BorrowsAnalyzed) {
        throw std::runtime_error(
            "ARC optimizer requires completed borrow inference");
    }
    const bool has_callable = std::ranges::any_of(
        program.declarations, is_callable);
    if (has_callable && program.ownership_summaries.empty()) {
        throw std::runtime_error(
            "ARC optimizer requires existing ownership summaries");
    }

    std::unordered_map<std::string, const IRDeclaration*> declarations;
    for (const auto& declaration : program.declarations) {
        declarations.emplace(declaration.identity, &declaration);
    }

    std::size_t elided_pairs = 0;
    std::size_t transfer_count = 0;
    std::size_t borrow_count = 0;
    for (auto& schedule : program.arc_schedules) {
        schedule.optimized_operations = schedule.unoptimized_operations;
        const auto declaration = declarations.find(schedule.callable_identity);
        if (declaration == declarations.end() || !declaration->second->body) {
            continue;
        }
        apply_transfer_operations(
            *declaration->second->body, schedule, transfer_count);
        const auto callable_summary = std::ranges::find_if(
            program.ownership_summaries,
            [&](const auto& summary) {
                return summary.callable_identity == schedule.callable_identity;
            });
        if (program.borrowing_enabled
            && callable_summary != program.ownership_summaries.end()) {
            for (const auto& summary : callable_summary->parameters) {
                if (!ownership_is_observed_only(summary)) continue;
                std::size_t pair = 0;
                for (const auto& operation : schedule.optimized_operations) {
                    if (operation.kind == IRArcOperationKind::Retain
                        && operation.value == summary.parameter_name
                        && operation.reason == "owned parameter entry") {
                        pair = operation.pair_id;
                        break;
                    }
                }
                elide_pair(schedule, pair, elided_pairs);
                schedule.optimized_operations.push_back(IRArcOperation{
                    IRArcOperationKind::BorrowedTemporary,
                    summary.parameter_name,
                    summary.origin,
                    summary.receiver
                        ? "inferred operation-bounded receiver borrow"
                        : "inferred operation-bounded parameter borrow",
                    pair,
                    false,
                });
                ++borrow_count;
            }
        }
        apply_borrow_operations(*declaration->second->body, schedule,
            borrow_count, elided_pairs);
    }
    if (!program.top_level_operations.empty()) {
        const auto schedule = std::ranges::find_if(
            program.arc_schedules,
            [](const auto& candidate) {
                return candidate.callable_identity == "<top-level>";
            });
        if (schedule != program.arc_schedules.end()) {
            for (const auto& operation : program.top_level_operations) {
                apply_transfer_operations(operation, *schedule, transfer_count);
            }
        }
    }
    program.arc_stage = IRArcStage::Optimized;
    update_metrics(program);
    program.arc_metrics.elided_pairs = elided_pairs;
    program.arc_metrics.ownership_transfers = transfer_count;
    program.arc_metrics.borrows_inferred = borrow_count;
}

} // namespace toro
