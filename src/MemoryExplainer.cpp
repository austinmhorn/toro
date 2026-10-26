#include "toro/MemoryExplainer.hpp"

#include <algorithm>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

namespace toro {
namespace {

bool same_origin(const IROrigin& left, const IROrigin& right)
{
    return left.module_name == right.module_name
        && left.location.line == right.location.line
        && left.location.column == right.location.column;
}

std::string origin_text(const IROrigin& origin)
{
    return origin.module_name + ':' + std::to_string(origin.location.line)
        + ':' + std::to_string(origin.location.column);
}

std::string_view explanation_kind_name(MemoryExplanationKind kind)
{
    switch (kind) {
    case MemoryExplanationKind::Allocation: return "allocation";
    case MemoryExplanationKind::Escape: return "escape";
    case MemoryExplanationKind::ArcKept: return "arc-kept";
    case MemoryExplanationKind::ArcElided: return "arc-elided";
    case MemoryExplanationKind::ArcOperation: return "arc-operation";
    case MemoryExplanationKind::Borrow: return "borrow";
    case MemoryExplanationKind::Transfer: return "ownership-transfer";
    case MemoryExplanationKind::StrongField: return "strong-field";
    case MemoryExplanationKind::WeakField: return "weak-field";
    case MemoryExplanationKind::PossibleStrongCycle:
        return "possible-strong-cycle";
    }
    return "unknown";
}

std::string_view arc_operation_name(IRArcOperationKind kind)
{
    switch (kind) {
    case IRArcOperationKind::Retain: return "retain";
    case IRArcOperationKind::Release: return "release";
    case IRArcOperationKind::DestroyFinalize: return "destroy-finalize";
    case IRArcOperationKind::ManagedAssignment: return "managed-assignment";
    case IRArcOperationKind::OwnedTemporary: return "owned-temporary";
    case IRArcOperationKind::BorrowedTemporary: return "borrow";
    case IRArcOperationKind::OwnershipTransfer: return "ownership-transfer";
    }
    return "unknown";
}

bool memory_field(const IRField& field)
{
    return field.is_weak
        || field.type.category == IRValueCategory::ManagedReference
        || field.type.category == IRValueCategory::InterfaceValue;
}

std::string escape_decision(const IREscapeCause& cause)
{
    if (cause.uncertain) return "escape-unknown";
    return cause.causes_escape ? "escapes" : "non-escaping-observation";
}

MemoryExplanation arc_explanation(const CallableMemoryReport& callable,
    const MemoryArcOperationReport& operation, MemoryExplanationKind kind,
    std::string decision)
{
    MemoryExplanation explanation;
    explanation.kind = kind;
    explanation.origin = operation.origin;
    explanation.subject_identity = callable.callable_identity + "::"
        + operation.value;
    explanation.decision = std::move(decision);
    explanation.arc_operation = operation.kind;
    explanation.reasons.push_back(MemoryExplanationReason{
        "arc-operation", operation.reason, operation.origin,
        callable.callable_identity});
    if (operation.elision) {
        explanation.reasons.push_back(MemoryExplanationReason{
            operation.elision->kind == IRArcElisionKind::InferredBorrow
                ? "inferred-borrow" : "ownership-transfer",
            operation.elision->reason,
            operation.elision->origin,
            operation.elision->related_value,
        });
    }
    if (operation.retention) {
        explanation.reasons.push_back(MemoryExplanationReason{
            "arc-retention", operation.retention->reason,
            operation.retention->origin, callable.callable_identity,
        });
    }
    return explanation;
}

void add_dynamic_targets(
    const IRProgram& program, MemoryExplanation& explanation)
{
    for (const auto& summary : program.effect_summaries) {
        for (const auto& edge : summary.call_edges) {
            if (!same_origin(edge.origin, explanation.origin)
                || (edge.operation_kind != IROperationKind::VirtualCall
                    && edge.operation_kind
                        != IROperationKind::InterfaceCall)) {
                continue;
            }
            explanation.reasons.push_back(MemoryExplanationReason{
                edge.operation_kind == IROperationKind::VirtualCall
                    ? "reachable-virtual-target"
                    : "reachable-interface-target",
                edge.operation_name,
                edge.origin,
                edge.callable_identity,
            });
        }
    }
}

void sort_reasons(std::vector<MemoryExplanationReason>& reasons)
{
    std::ranges::sort(reasons,
        [](const auto& left, const auto& right) {
            const auto left_origin = left.origin
                ? std::tuple{left.origin->module_name,
                    left.origin->location.line,
                    left.origin->location.column}
                : std::tuple{std::string{}, std::size_t{}, std::size_t{}};
            const auto right_origin = right.origin
                ? std::tuple{right.origin->module_name,
                    right.origin->location.line,
                    right.origin->location.column}
                : std::tuple{std::string{}, std::size_t{}, std::size_t{}};
            return std::tie(left.code, left.related_identity, left.detail,
                       left_origin)
                < std::tie(right.code, right.related_identity, right.detail,
                       right_origin);
        });
    reasons.erase(std::ranges::unique(reasons,
        [](const auto& left, const auto& right) {
            const bool origins_equal = (!left.origin && !right.origin)
                || (left.origin && right.origin
                    && same_origin(*left.origin, *right.origin));
            return left.code == right.code && left.detail == right.detail
                && left.related_identity == right.related_identity
                && origins_equal;
        }).begin(), reasons.end());
}

std::vector<MemoryExplanation> collect_facts(
    const IRProgram& program, const MemoryReport& memory)
{
    std::vector<MemoryExplanation> facts;
    for (const auto& allocation : memory.allocations) {
        MemoryExplanation explanation;
        explanation.kind = MemoryExplanationKind::Allocation;
        explanation.origin = allocation.origin;
        explanation.subject_identity = allocation.identity;
        explanation.decision = allocation.storage == IRStorageClass::Stack
            ? "stack-storage" : "heap-storage";
        explanation.type = allocation.type;
        explanation.storage = allocation.storage;
        explanation.escape_state = allocation.escape_state;
        explanation.reasons.push_back(MemoryExplanationReason{
            "storage-decision", allocation.storage_reason, allocation.origin,
            allocation.callable_identity});
        explanation.reasons.push_back(MemoryExplanationReason{
            "escape-state",
            std::string(ir_escape_state_name(allocation.escape_state)),
            allocation.origin, allocation.callable_identity});
        for (const auto& cause : allocation.escape_causes) {
            explanation.reasons.push_back(MemoryExplanationReason{
                std::string(ir_escape_reason_name(cause.reason)), cause.detail,
                cause.origin, allocation.callable_identity});

            MemoryExplanation escape;
            escape.kind = MemoryExplanationKind::Escape;
            escape.origin = cause.origin;
            escape.subject_identity = allocation.identity;
            escape.decision = escape_decision(cause);
            escape.type = allocation.type;
            escape.storage = allocation.storage;
            escape.escape_state = allocation.escape_state;
            escape.reasons.push_back(MemoryExplanationReason{
                std::string(ir_escape_reason_name(cause.reason)), cause.detail,
                cause.origin, allocation.identity});
            escape.reasons.push_back(MemoryExplanationReason{
                "allocation-site", "related managed construction",
                allocation.origin, allocation.identity});
            facts.push_back(std::move(escape));
        }
        facts.push_back(std::move(explanation));
    }

    for (const auto& callable : memory.callables) {
        for (const auto& operation : callable.final_arc_operations) {
            const bool retain_or_release = operation.kind
                    == IRArcOperationKind::Retain
                || operation.kind == IRArcOperationKind::Release;
            auto explanation = arc_explanation(callable, operation,
                retain_or_release ? MemoryExplanationKind::ArcKept
                                  : MemoryExplanationKind::ArcOperation,
                std::string(retain_or_release ? "kept-" : "final-")
                    + std::string(arc_operation_name(operation.kind)));
            add_dynamic_targets(program, explanation);
            facts.push_back(std::move(explanation));
        }
        for (const auto& operation : callable.elided_arc_operations) {
            facts.push_back(arc_explanation(callable, operation,
                MemoryExplanationKind::ArcElided,
                "elided-" + std::string(arc_operation_name(operation.kind))));
        }
        for (const auto& operation : callable.ownership_transfers) {
            facts.push_back(arc_explanation(callable, operation,
                MemoryExplanationKind::Transfer, "ownership-transfer"));
        }
        for (const auto& operation : callable.inferred_borrows) {
            auto explanation = arc_explanation(callable, operation,
                MemoryExplanationKind::Borrow, "borrow");
            add_dynamic_targets(program, explanation);
            facts.push_back(std::move(explanation));
        }
    }

    for (const auto& declaration : program.declarations) {
        for (const auto& field : declaration.fields) {
            if (!memory_field(field)) continue;
            MemoryExplanation explanation;
            explanation.kind = field.is_weak
                ? MemoryExplanationKind::WeakField
                : MemoryExplanationKind::StrongField;
            explanation.origin = field.origin;
            explanation.subject_identity = declaration.identity + '.' + field.name;
            explanation.decision = field.is_weak
                ? "weak-non-owning-field" : "strong-owning-field";
            explanation.type = field.type;
            explanation.reasons.push_back(MemoryExplanationReason{
                field.is_weak ? "explicit-weak-declaration"
                              : "default-strong-ownership",
                field.is_weak
                    ? "source declaration explicitly uses weak"
                    : "class references are strong by default",
                field.origin, declaration.identity});
            facts.push_back(std::move(explanation));
        }
    }

    for (const auto& cycle : memory.cycles) {
        for (const auto& edge : cycle.edges) {
            MemoryExplanation explanation;
            explanation.kind = MemoryExplanationKind::PossibleStrongCycle;
            explanation.origin = edge.origin;
            explanation.subject_identity = edge.owner_type + '.' + edge.field_path;
            explanation.decision = "possible-strong-reference-cycle";
            explanation.reasons.push_back(MemoryExplanationReason{
                "strong-ownership-edge",
                edge.owner_display + '.' + edge.field_path + " -> "
                    + edge.target_display,
                edge.origin, edge.target_type});
            facts.push_back(std::move(explanation));
        }
    }

    for (auto& fact : facts) sort_reasons(fact.reasons);
    std::ranges::sort(facts,
        [](const auto& left, const auto& right) {
            return std::tie(left.origin.module_name, left.origin.location.line,
                       left.origin.location.column, left.kind,
                       left.subject_identity, left.decision)
                < std::tie(right.origin.module_name, right.origin.location.line,
                       right.origin.location.column, right.kind,
                       right.subject_identity, right.decision);
        });
    return facts;
}

std::string query_text(const MemoryExplanationResult& result)
{
    std::string text = result.module_name + ':' + std::to_string(result.line);
    if (result.column) text += ':' + std::to_string(*result.column);
    return text;
}

} // namespace

MemoryExplanationResult MemoryExplainer::explain(
    const IRProgram& program, const MemoryReport& memory,
    std::string module_name, std::size_t line,
    std::optional<std::size_t> column) const
{
    MemoryExplanationResult result;
    result.module_name = std::move(module_name);
    result.line = line;
    result.column = column;

    auto facts = collect_facts(program, memory);
    std::erase_if(facts, [&](const auto& fact) {
        return fact.origin.module_name != result.module_name
            || fact.origin.location.line != result.line
            || (column && fact.origin.location.column != *column);
    });
    if (facts.empty()) return result;

    if (!column) {
        std::vector<std::size_t> columns;
        for (const auto& fact : facts) {
            columns.push_back(fact.origin.location.column);
        }
        std::ranges::sort(columns);
        columns.erase(std::ranges::unique(columns).begin(), columns.end());
        if (columns.size() > 1) {
            result.status = MemoryExplanationStatus::Ambiguous;
            for (const auto candidate_column : columns) {
                MemoryExplanationCandidate candidate;
                candidate.origin = IROrigin{
                    result.module_name,
                    SourceLocation{result.line, candidate_column}};
                for (const auto& fact : facts) {
                    if (fact.origin.location.column == candidate_column) {
                        candidate.kinds.push_back(fact.kind);
                    }
                }
                std::ranges::sort(candidate.kinds);
                candidate.kinds.erase(
                    std::ranges::unique(candidate.kinds).begin(),
                    candidate.kinds.end());
                result.candidates.push_back(std::move(candidate));
            }
            return result;
        }
    }

    result.status = MemoryExplanationStatus::Explained;
    result.explanations = std::move(facts);
    return result;
}

std::string MemoryExplainer::format(
    const MemoryExplanationResult& result) const
{
    std::string output = "toro-memory-explanation 1\n";
    output += "target " + query_text(result) + "\n";
    if (result.status == MemoryExplanationStatus::NoMemoryDecision) {
        output += "status no-memory-decision\n";
        output += "message no memory decision at " + query_text(result) + "\n";
        return output;
    }
    if (result.status == MemoryExplanationStatus::Ambiguous) {
        output += "status ambiguous\n";
        output += "candidates " + std::to_string(result.candidates.size()) + "\n";
        for (const auto& candidate : result.candidates) {
            output += "  " + origin_text(candidate.origin) + " ";
            for (std::size_t index = 0; index < candidate.kinds.size(); ++index) {
                if (index != 0) output += ',';
                output += explanation_kind_name(candidate.kinds[index]);
            }
            output += '\n';
        }
        output += "hint provide file:line:column for one source location\n";
        return output;
    }

    output += "status explained\n";
    output += "location " + origin_text(result.explanations.front().origin)
        + "\n";
    for (const auto& explanation : result.explanations) {
        output += "fact "
            + std::string(explanation_kind_name(explanation.kind)) + "\n";
        output += "  subject " + explanation.subject_identity + "\n";
        if (explanation.type) {
            output += "  type " + type_name(explanation.type->type) + "\n";
        }
        output += "  decision " + explanation.decision + "\n";
        if (explanation.storage) {
            output += "  storage "
                + std::string(ir_storage_class_name(*explanation.storage)) + "\n";
            output += "  general-heap-allocation "
                + std::string(*explanation.storage == IRStorageClass::Heap
                        ? "yes" : "no")
                + "\n";
        }
        if (explanation.escape_state) {
            output += "  escape "
                + std::string(ir_escape_state_name(*explanation.escape_state))
                + "\n";
        }
        if (explanation.arc_operation) {
            output += "  arc-operation "
                + std::string(arc_operation_name(*explanation.arc_operation))
                + "\n";
        }
        output += "  reasons\n";
        if (explanation.reasons.empty()) output += "    none\n";
        for (const auto& reason : explanation.reasons) {
            output += "    " + reason.code;
            if (!reason.related_identity.empty()) {
                output += " " + reason.related_identity;
            }
            if (reason.origin) output += " @" + origin_text(*reason.origin);
            if (!reason.detail.empty()) output += " ; " + reason.detail;
            output += '\n';
        }
    }
    return output;
}

} // namespace toro
