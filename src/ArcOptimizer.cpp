#include "toro/ArcOptimizer.hpp"

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
            append_operation(schedule, IRArcOperationKind::OwnedTemporary,
                operation.name, operation.origin, "class construction result");
        }
        break;
    case IROperationKind::Declare:
    case IROperationKind::Assign:
        if (operation.result_type && owns_runtime_storage(*operation.result_type)) {
            append_operation(schedule, IRArcOperationKind::ManagedAssignment,
                operation.name, operation.origin,
                operation.kind == IROperationKind::Declare
                    ? "managed local initialization"
                    : "managed local replacement");
            const auto pair = next_pair++;
            if (is_shared_reference(*operation.result_type)
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

bool has_barrier(const IRCallableEffectSummary& summary)
{
    for (const auto effect : summary.effects) {
        if (effect == IREffect::Escape || effect == IREffect::ForeignCall
            || effect == IREffect::DynamicDispatch
            || effect == IREffect::UnknownExternal) {
            return true;
        }
    }
    return false;
}

bool safe_parameter_load(
    const IROperation& parent,
    std::size_t child_index)
{
    if (parent.kind == IROperationKind::MemberAccess && child_index == 0
        && parent.result_type && !owns_runtime_storage(*parent.result_type)) {
        return true;
    }
    return parent.kind == IROperationKind::Binary
        && (parent.name == "==" || parent.name == "!=");
}

bool parameter_is_observed_only(
    const IROperation& operation,
    std::string_view parameter,
    const IROperation* parent = nullptr,
    std::size_t child_index = 0)
{
    if (operation.kind == IROperationKind::Load
        && operation.name == parameter) {
        return parent && safe_parameter_load(*parent, child_index);
    }
    for (std::size_t index = 0; index < operation.children.size(); ++index) {
        if (!parameter_is_observed_only(
                operation.children[index], parameter, &operation, index)) {
            return false;
        }
    }
    return true;
}

const IRCallableEffectSummary* find_summary(
    const IRProgram& program,
    std::string_view identity)
{
    const auto found = std::ranges::find_if(
        program.effect_summaries,
        [&](const auto& summary) {
            return summary.callable_identity == identity;
        });
    return found == program.effect_summaries.end() ? nullptr : &*found;
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

void ArcOptimizer::optimize(IRProgram& program) const
{
    if (program.arc_stage == IRArcStage::Semantic) {
        throw std::runtime_error(
            "ARC optimizer requires materialized lifetime IR");
    }
    const bool has_callable = std::ranges::any_of(
        program.declarations, is_callable);
    if (has_callable && program.effect_summaries.empty()) {
        throw std::runtime_error(
            "ARC optimizer requires existing effect summaries");
    }

    std::unordered_map<std::string, const IRDeclaration*> declarations;
    for (const auto& declaration : program.declarations) {
        declarations.emplace(declaration.identity, &declaration);
    }

    std::size_t elided_pairs = 0;
    for (auto& schedule : program.arc_schedules) {
        schedule.optimized_operations = schedule.unoptimized_operations;
        const auto declaration = declarations.find(schedule.callable_identity);
        if (declaration == declarations.end() || !declaration->second->body
            || !declaration->second->generic_parameters.empty()) {
            continue;
        }
        const auto* effects = find_summary(program, schedule.callable_identity);
        if (!effects || has_barrier(*effects)) continue;

        for (const auto& parameter : declaration->second->parameters) {
            if (!is_shared_reference(parameter.type)
                || !parameter_is_observed_only(
                    *declaration->second->body, parameter.name)) {
                continue;
            }
            std::size_t pair = 0;
            for (const auto& operation : schedule.optimized_operations) {
                if (operation.kind == IRArcOperationKind::Retain
                    && operation.value == parameter.name
                    && operation.reason == "owned parameter entry") {
                    pair = operation.pair_id;
                    break;
                }
            }
            if (pair == 0) continue;
            for (auto& operation : schedule.optimized_operations) {
                if (operation.pair_id == pair) operation.elided = true;
            }
            schedule.optimized_operations.push_back(IRArcOperation{
                IRArcOperationKind::BorrowedTemporary,
                parameter.name,
                parameter.origin,
                "locally proven non-escaping parameter borrow",
                pair,
                false,
            });
            ++elided_pairs;
        }
    }
    program.arc_stage = IRArcStage::Optimized;
    update_metrics(program);
    program.arc_metrics.elided_pairs = elided_pairs;
}

} // namespace toro
