#include "toro/IR.hpp"

#include <algorithm>
#include <limits>
#include <string>
#include <string_view>

namespace toro {
namespace {

std::string_view category_name(IRValueCategory category)
{
    switch (category) {
    case IRValueCategory::Primitive: return "primitive";
    case IRValueCategory::Value: return "value";
    case IRValueCategory::ManagedReference: return "managed-ref";
    case IRValueCategory::InterfaceValue: return "interface";
    case IRValueCategory::CollectionValue: return "collection";
    case IRValueCategory::ResultValue: return "result";
    case IRValueCategory::Unknown: return "unknown";
    }
    return "unknown";
}

std::string_view operation_name(IROperationKind kind)
{
    switch (kind) {
    case IROperationKind::Block: return "block";
    case IROperationKind::Constant: return "constant";
    case IROperationKind::Load: return "load";
    case IROperationKind::Declare: return "declare";
    case IROperationKind::Assign: return "assign";
    case IROperationKind::MemberAssign: return "member-assign";
    case IROperationKind::IndexAssign: return "index-assign";
    case IROperationKind::Unary: return "unary";
    case IROperationKind::Binary: return "binary";
    case IROperationKind::Call: return "call";
    case IROperationKind::ForeignCall: return "foreign-call";
    case IROperationKind::VirtualCall: return "virtual-call";
    case IROperationKind::InterfaceCall: return "interface-call";
    case IROperationKind::Construction: return "construct";
    case IROperationKind::EnumConstruction: return "enum-construct";
    case IROperationKind::ResultConstruction: return "result-construct";
    case IROperationKind::CollectionOperation: return "collection-op";
    case IROperationKind::MemberAccess: return "member-access";
    case IROperationKind::IndexAccess: return "index-access";
    case IROperationKind::Conversion: return "convert";
    case IROperationKind::PropagateResult: return "propagate-result";
    case IROperationKind::Branch: return "branch";
    case IROperationKind::Loop: return "loop";
    case IROperationKind::ForEach: return "for-each";
    case IROperationKind::Handle: return "handle";
    case IROperationKind::Return: return "return";
    case IROperationKind::Stop: return "stop";
    case IROperationKind::Continue: return "continue";
    }
    return "unknown";
}

std::string_view declaration_kind_name(IRDeclarationKind kind)
{
    switch (kind) {
    case IRDeclarationKind::Function: return "function";
    case IRDeclarationKind::ForeignFunction: return "foreign-function";
    case IRDeclarationKind::Struct: return "struct";
    case IRDeclarationKind::Class: return "class";
    case IRDeclarationKind::Interface: return "interface";
    case IRDeclarationKind::Enum: return "enum";
    case IRDeclarationKind::Method: return "method";
    case IRDeclarationKind::Conversion: return "conversion";
    }
    return "unknown";
}

std::string_view arc_operation_name(IRArcOperationKind kind)
{
    switch (kind) {
    case IRArcOperationKind::Retain: return "retain";
    case IRArcOperationKind::Release: return "release";
    case IRArcOperationKind::DestroyFinalize: return "destroy-finalize";
    case IRArcOperationKind::ManagedAssignment: return "managed-assign";
    case IRArcOperationKind::OwnedTemporary: return "owned-temporary";
    case IRArcOperationKind::BorrowedTemporary: return "borrowed-temporary";
    case IRArcOperationKind::OwnershipTransfer: return "transfer";
    }
    return "unknown";
}

std::string complete_type_name(const IRType& type)
{
    std::string result = type_name(type.type);
    if (type.type.kind == TypeKind::Unknown && !type.type.name.empty()) {
        result += " {" + type.type.name + '}';
    }
    result += " [" + std::string(category_name(type.category)) + ']';
    return result;
}

void append_origin(const IROrigin& origin, std::string& output)
{
    output += " @";
    output += origin.module_name.empty() ? "<direct>" : origin.module_name;
    output += ':' + std::to_string(origin.location.line);
    output += ':' + std::to_string(origin.location.column);
}

void append_operation(
    const IROperation& operation,
    std::size_t depth,
    std::string& output)
{
    output += std::string(depth * 2, ' ');
    output += operation_name(operation.kind);
    if (!operation.name.empty()) {
        output += ' ' + operation.name;
    }
    if (operation.result_type) {
        output += " : " + complete_type_name(*operation.result_type);
    }
    if (!operation.selected_declaration.empty()) {
        output += " -> " + operation.selected_declaration;
    }
    if (!operation.substitutions.empty()) {
        output += " <";
        for (std::size_t index = 0; index < operation.substitutions.size(); ++index) {
            if (index != 0) output += ", ";
            output += operation.substitutions[index].first + '='
                + complete_type_name(operation.substitutions[index].second);
        }
        output += '>';
    }
    if (!operation.argument_names.empty()) {
        output += " arguments(";
        for (std::size_t index = 0; index < operation.argument_names.size(); ++index) {
            if (index != 0) output += ", ";
            output += operation.argument_names[index].empty()
                ? "<positional>" : operation.argument_names[index];
        }
        output += ')';
    }
    append_origin(operation.origin, output);
    output += '\n';
    for (const auto& child : operation.children) {
        append_operation(child, depth + 1, output);
    }
}

} // namespace

std::string_view ir_effect_name(IREffect effect)
{
    switch (effect) {
    case IREffect::Allocate: return "allocate";
    case IREffect::Retain: return "retain";
    case IREffect::ReleaseDestroy: return "release_destroy";
    case IREffect::Escape: return "escape";
    case IREffect::Block: return "block";
    case IREffect::FilesystemIO: return "filesystem_io";
    case IREffect::EnvironmentAccess: return "environment_access";
    case IREffect::TimeQuery: return "time_query";
    case IREffect::ProcessIO: return "process_io";
    case IREffect::Synchronize: return "synchronize";
    case IREffect::ForeignCall: return "foreign_call";
    case IREffect::NativeCall: return "native_call";
    case IREffect::DynamicDispatch: return "dynamic_dispatch";
    case IREffect::Terminate: return "terminate";
    case IREffect::UnknownExternal: return "unknown_external";
    case IREffect::Count: break;
    }
    return "unknown";
}

std::string_view ir_escape_state_name(IREscapeState state)
{
    switch (state) {
    case IREscapeState::DoesNotEscape: return "no";
    case IREscapeState::Escapes: return "yes";
    case IREscapeState::Unknown: return "unknown";
    }
    return "unknown";
}

std::string_view ir_escape_reason_name(IREscapeReason reason)
{
    switch (reason) {
    case IREscapeReason::Return: return "return";
    case IREscapeReason::GlobalStorage: return "global-storage";
    case IREscapeReason::StrongField: return "strong-field";
    case IREscapeReason::CollectionStorage: return "collection-storage";
    case IREscapeReason::CallArgument: return "call-argument";
    case IREscapeReason::DynamicCall: return "dynamic-call";
    case IREscapeReason::ForeignCall: return "foreign-call";
    case IREscapeReason::UnknownCall: return "unknown-call";
    case IREscapeReason::WeakObservation: return "weak-observation";
    case IREscapeReason::InterfaceAlias: return "interface-alias";
    case IREscapeReason::ResultPayload: return "result-payload";
    case IREscapeReason::EnumPayload: return "enum-payload";
    }
    return "unknown";
}

std::string_view ir_storage_class_name(IRStorageClass storage)
{
    switch (storage) {
    case IRStorageClass::Heap: return "heap";
    case IRStorageClass::Stack: return "stack";
    }
    return "heap";
}

std::string_view ir_reference_mode_name(IRReferenceMode mode)
{
    switch (mode) {
    case IRReferenceMode::Own: return "own";
    case IRReferenceMode::Transfer: return "transfer";
    case IRReferenceMode::Borrow: return "borrow";
    case IRReferenceMode::Weak: return "weak";
    }
    return "own";
}

std::string_view ir_borrow_kind_name(IRBorrowKind kind)
{
    switch (kind) {
    case IRBorrowKind::Parameter: return "parameter";
    case IRBorrowKind::Receiver: return "receiver";
    case IRBorrowKind::CallArgument: return "argument";
    case IRBorrowKind::LocalAlias: return "local-alias";
    }
    return "argument";
}

std::string_view ir_ownership_behavior_name(IROwnershipBehavior behavior)
{
    switch (behavior) {
    case IROwnershipBehavior::Observe: return "borrow/no-escape";
    case IROwnershipBehavior::Retain: return "retained";
    case IROwnershipBehavior::Store: return "stored";
    case IROwnershipBehavior::Return: return "returned";
    case IROwnershipBehavior::Consume: return "consumed";
    case IROwnershipBehavior::Escape: return "escapes";
    case IROwnershipBehavior::WeakStore: return "weak-stored";
    case IROwnershipBehavior::Unknown: return "unknown";
    }
    return "unknown";
}

std::string_view ir_return_ownership_name(IRReturnOwnership ownership)
{
    switch (ownership) {
    case IRReturnOwnership::None: return "none";
    case IRReturnOwnership::NewOwned: return "new-owned";
    case IRReturnOwnership::ParameterAlias: return "parameter-alias";
    case IRReturnOwnership::ReceiverAlias: return "receiver-alias";
    case IRReturnOwnership::FieldAlias: return "field-alias";
    case IRReturnOwnership::Unknown: return "unknown";
    }
    return "unknown";
}

std::string dump_ir(
    const IRProgram& program,
    bool include_effects,
    bool include_arc,
    bool include_escape,
    bool include_ownership)
{
    std::string output = "toro-ir 1\n";
    output += "entry " + program.entry_module + "\n";
    output += program.lifetime_model == IRLifetimeModel::LegacyArc
        ? "lifetime legacy-arc\n" : "lifetime explicit-arc\n";
    for (const auto& module : program.modules) {
        output += "module " + module.name;
        if (!module.imports.empty()) {
            output += " imports ";
            for (std::size_t index = 0; index < module.imports.size(); ++index) {
                if (index != 0) output += ", ";
                output += module.imports[index];
            }
        }
        output += '\n';
    }
    for (const auto& operation : program.top_level_operations) {
        append_operation(operation, 0, output);
    }
    for (const auto& declaration : program.declarations) {
        output += std::string(declaration.owner_identity.empty() ? 0 : 2, ' ');
        output += declaration_kind_name(declaration.kind);
        output += ' ' + declaration.identity;
        if (!declaration.owner_identity.empty()) {
            output += " owner " + declaration.owner_identity;
        }
        if (!declaration.generic_parameters.empty()) {
            output += " generic <";
            for (std::size_t index = 0;
                 index < declaration.generic_parameters.size(); ++index) {
                if (index != 0) output += ", ";
                const auto& parameter = declaration.generic_parameters[index];
                output += parameter.name;
                if (!parameter.constraints.empty()) {
                    output += ": ";
                    for (std::size_t constraint = 0;
                         constraint < parameter.constraints.size(); ++constraint) {
                        if (constraint != 0) output += " + ";
                        output += complete_type_name(parameter.constraints[constraint]);
                    }
                }
            }
            output += '>';
        }
        if (!declaration.parameters.empty()) {
            output += " (";
            for (std::size_t index = 0; index < declaration.parameters.size(); ++index) {
                if (index != 0) output += ", ";
                output += declaration.parameters[index].name + ": "
                    + complete_type_name(declaration.parameters[index].type);
            }
            output += ')';
        }
        if (declaration.return_type) {
            output += " -> " + complete_type_name(*declaration.return_type);
        }
        if (declaration.is_abstract) output += " abstract";
        if (declaration.is_virtual) output += " virtual";
        if (declaration.is_override) output += " override";
        if (declaration.is_public) output += " public";
        append_origin(declaration.origin, output);
        output += '\n';
        for (const auto& field : declaration.fields) {
            output += "  field " + field.name + ": "
                + complete_type_name(field.type);
            if (field.is_public) output += " public";
            if (field.is_weak) output += " weak";
            append_origin(field.origin, output);
            output += '\n';
            if (field.default_value) {
                append_operation(*field.default_value, 2, output);
            }
        }
        for (const auto& variant : declaration.variants) {
            output += "  variant " + variant.name;
            if (variant.payload_type) {
                output += ": " + complete_type_name(*variant.payload_type);
            }
            append_origin(variant.origin, output);
            output += '\n';
        }
        for (const auto& related : declaration.related_types) {
            output += "  relates " + complete_type_name(related) + '\n';
        }
        if (declaration.body) {
            append_operation(*declaration.body, 1, output);
        }
    }
    if (include_effects) {
        output += "effects\n";
        for (const auto& summary : program.effect_summaries) {
            output += "  callable " + summary.callable_identity + '\n';
            if (summary.effects.empty()) {
                output += "    none\n";
                continue;
            }
            for (const auto effect : summary.effects) {
                output += "    " + std::string(ir_effect_name(effect));
                for (const auto& cause : summary.causes) {
                    if (cause.effect != effect) continue;
                    if (cause.transitive) {
                        output += " via " + cause.via_callable;
                    }
                    append_origin(cause.origin, output);
                    break;
                }
                output += '\n';
            }
        }
    }
    if (include_ownership) {
        output += "ownership\n";
        for (const auto& summary : program.ownership_summaries) {
            output += "  callable " + summary.callable_identity + '\n';
            if (summary.parameters.empty()) output += "    parameters none\n";
            for (const auto& parameter : summary.parameters) {
                output += "    "
                    + std::string(parameter.receiver ? "receiver " : "param ")
                    + parameter.parameter_name + '\n';
                for (const auto behavior : parameter.behaviors) {
                    output += "      "
                        + std::string(ir_ownership_behavior_name(behavior));
                    const auto cause = std::ranges::find_if(
                        parameter.causes,
                        [&](const auto& candidate) {
                            return candidate.behavior == behavior;
                        });
                    if (cause != parameter.causes.end()) {
                        if (cause->transitive && !cause->via_callable.empty()) {
                            output += " via " + cause->via_callable;
                        }
                        if (!cause->reason.empty()) {
                            output += " ; " + cause->reason;
                        }
                        append_origin(cause->origin, output);
                    }
                    output += '\n';
                }
            }
            output += "    return "
                + std::string(ir_return_ownership_name(
                    summary.return_value.ownership));
            if (summary.return_value.parameter_index) {
                if (*summary.return_value.parameter_index
                    == std::numeric_limits<std::size_t>::max()) {
                    output += " from receiver";
                } else {
                    output += " from-index "
                        + std::to_string(*summary.return_value.parameter_index);
                }
            }
            if (!summary.return_value.reason.empty()) {
                output += " ; " + summary.return_value.reason;
                append_origin(summary.return_value.origin, output);
            }
            output += '\n';
        }
    }
    if (include_escape) {
        output += "escape\n";
        output += "  metrics managed-allocations "
            + std::to_string(program.escape_metrics.managed_allocations)
            + " non-escaping "
            + std::to_string(program.escape_metrics.non_escaping)
            + " escaping "
            + std::to_string(program.escape_metrics.escaping)
            + " unknown "
            + std::to_string(program.escape_metrics.unknown)
            + " stack "
            + std::to_string(program.escape_metrics.stack_allocations)
            + " heap "
            + std::to_string(program.escape_metrics.heap_allocations) + "\n";
        for (const auto& allocation : program.escape_allocations) {
            output += "  allocation " + allocation.identity + " : "
                + complete_type_name(allocation.type);
            append_origin(allocation.origin, output);
            output += "\n    escape "
                + std::string(ir_escape_state_name(allocation.state)) + '\n';
            output += "    storage "
                + std::string(ir_storage_class_name(allocation.storage))
                + " ; " + allocation.storage_reason + '\n';
            for (const auto& cause : allocation.causes) {
                output += cause.causes_escape ? "    reason " : "    constraint ";
                output += ir_escape_reason_name(cause.reason);
                if (cause.uncertain) output += " uncertain";
                if (!cause.detail.empty()) output += " " + cause.detail;
                append_origin(cause.origin, output);
                output += '\n';
            }
        }
    }
    if (include_arc) {
        output += "arc\n";
        output += "  stage ";
        switch (program.arc_stage) {
        case IRArcStage::Semantic: output += "semantic\n"; break;
        case IRArcStage::Materialized: output += "materialized\n"; break;
        case IRArcStage::TransfersAnalyzed:
            output += "transfers-analyzed\n";
            break;
        case IRArcStage::BorrowsAnalyzed:
            output += "borrows-analyzed\n";
            break;
        case IRArcStage::Optimized: output += "optimized\n"; break;
        }
        output += "  metrics retains-before "
            + std::to_string(program.arc_metrics.retains_before)
            + " retains-after "
            + std::to_string(program.arc_metrics.retains_after)
            + " releases-before "
            + std::to_string(program.arc_metrics.releases_before)
            + " releases-after "
            + std::to_string(program.arc_metrics.releases_after)
            + " elided-pairs "
            + std::to_string(program.arc_metrics.elided_pairs)
            + " ownership-transfers "
            + std::to_string(program.arc_metrics.ownership_transfers)
            + " borrows-inferred "
            + std::to_string(program.arc_metrics.borrows_inferred) + "\n";
        for (const auto& schedule : program.arc_schedules) {
            output += "  callable " + schedule.callable_identity + "\n";
            output += "    before\n";
            for (const auto& operation : schedule.unoptimized_operations) {
                output += "      " + std::string(arc_operation_name(operation.kind));
                if (!operation.value.empty()) output += ' ' + operation.value;
                if (!operation.reason.empty()) output += " ; " + operation.reason;
                append_origin(operation.origin, output);
                output += '\n';
            }
            output += "    after\n";
            for (const auto& operation : schedule.optimized_operations) {
                output += "      " + std::string(arc_operation_name(operation.kind));
                if (!operation.value.empty()) output += ' ' + operation.value;
                if (operation.elided) output += " [elided]";
                if (!operation.reason.empty()) output += " ; " + operation.reason;
                append_origin(operation.origin, output);
                output += '\n';
            }
        }
    }
    return output;
}

} // namespace toro
