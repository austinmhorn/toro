#include "toro/EscapeAnalyzer.hpp"

#include <algorithm>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace toro {
namespace {

using AllocationSet = std::set<std::size_t>;

struct AliasState {
    std::unordered_map<std::string, AllocationSet> values;
};

bool is_managed_reference(const IRType& type)
{
    return type.category == IRValueCategory::ManagedReference;
}

bool is_reference_wrapper(const IRType& type)
{
    return type.category == IRValueCategory::ManagedReference
        || type.category == IRValueCategory::InterfaceValue;
}

void merge_into(AllocationSet& destination, const AllocationSet& source)
{
    destination.insert(source.begin(), source.end());
}

bool same_origin(const IROrigin& left, const IROrigin& right)
{
    return left.module_name == right.module_name
        && left.location.line == right.location.line
        && left.location.column == right.location.column;
}

std::string type_key(const Type& type)
{
    std::string result = type.kind == TypeKind::Unknown && !type.name.empty()
        ? type.name : std::string(base_type_name(type));
    if (!type.arguments.empty()) {
        result += '<';
        for (std::size_t index = 0; index < type.arguments.size(); ++index) {
            if (index != 0) result += ',';
            result += type_key(type.arguments[index]);
        }
        result += '>';
    }
    if (type.nullable) result += '?';
    return result;
}

const IRType& substitute_type(
    const IRType& type,
    const std::vector<std::pair<std::string, IRType>>& substitutions)
{
    if (type.type.kind != TypeKind::Unknown) return type;
    for (const auto& [name, concrete] : substitutions) {
        if (type.type.name == name) return concrete;
    }
    return type;
}

std::string specialization_identity(const IROperation& operation)
{
    std::string result = operation.selected_declaration;
    if (operation.substitutions.empty()) return result;
    result += '<';
    for (std::size_t index = 0; index < operation.substitutions.size(); ++index) {
        if (index != 0) result += ',';
        result += operation.substitutions[index].first + '='
            + type_key(substitute_type(
                operation.substitutions[index].second,
                operation.substitutions).type);
    }
    result += '>';
    return result;
}

bool has_effect(const IRCallableEffectSummary& summary, IREffect effect)
{
    return std::ranges::find(summary.effects, effect) != summary.effects.end();
}

class Analysis {
public:
    explicit Analysis(IRProgram& program) : program_(program)
    {
        for (const auto& declaration : program_.declarations) {
            declarations_.emplace(declaration.identity, &declaration);
        }
        for (const auto& summary : program_.effect_summaries) {
            summaries_.emplace(summary.callable_identity, &summary);
        }
    }

    void run()
    {
        program_.escape_allocations.clear();
        program_.escape_metrics = {};
        for (const auto& declaration : program_.declarations) {
            if (!declaration.body) continue;
            AliasState aliases;
            analyze_block(*declaration.body, aliases, declaration.identity);
        }
        AliasState top_level;
        for (const auto& operation : program_.top_level_operations) {
            analyze_statement(operation, top_level, "<top-level>");
            if (operation.kind == IROperationKind::Declare
                || operation.kind == IROperationKind::Assign) {
                const auto found = top_level.values.find(operation.name);
                if (found != top_level.values.end()) {
                    add_cause(found->second, IREscapeReason::GlobalStorage,
                        operation.origin, operation.name, true);
                }
            }
        }
        publish_metrics();
    }

private:
    std::size_t add_allocation(
        const IROperation& operation,
        std::string_view callable)
    {
        if (const auto existing = allocation_sites_.find(&operation);
            existing != allocation_sites_.end()) {
            return existing->second;
        }
        const std::size_t index = program_.escape_allocations.size();
        const auto& type = *operation.result_type;
        const std::string site = std::string(callable) + '@'
            + operation.origin.module_name + ':'
            + std::to_string(operation.origin.location.line) + ':'
            + std::to_string(operation.origin.location.column);
        const std::size_t occurrence = allocation_occurrences_[site]++;
        program_.escape_allocations.push_back(IRAllocationEscape{
            site + "#" + std::to_string(occurrence),
            std::string(callable),
            type,
            operation.origin,
            IREscapeState::DoesNotEscape,
            {},
        });
        allocation_sites_.emplace(&operation, index);
        return index;
    }

    void add_cause(
        const AllocationSet& allocations,
        IREscapeReason reason,
        const IROrigin& origin,
        std::string detail,
        bool causes_escape,
        bool uncertain = false)
    {
        for (const auto index : allocations) {
            auto& allocation = program_.escape_allocations.at(index);
            const bool duplicate = std::ranges::any_of(
                allocation.causes,
                [&](const IREscapeCause& cause) {
                    return cause.reason == reason && same_origin(cause.origin, origin)
                        && cause.detail == detail
                        && cause.causes_escape == causes_escape
                        && cause.uncertain == uncertain;
                });
            if (!duplicate) {
                allocation.causes.push_back(IREscapeCause{
                    reason, origin, detail, causes_escape, uncertain});
            }
            if (!causes_escape) continue;
            if (!uncertain) {
                allocation.state = IREscapeState::Escapes;
            } else if (allocation.state == IREscapeState::DoesNotEscape) {
                allocation.state = IREscapeState::Unknown;
            }
        }
    }

    std::optional<bool> weak_field(const IROperation& operation) const
    {
        if (operation.children.empty()
            || !operation.children.front().result_type) {
            return std::nullopt;
        }
        std::string owner = operation.children.front().result_type->type.name;
        while (!owner.empty()) {
            const auto declaration = declarations_.find(owner);
            if (declaration == declarations_.end()) return std::nullopt;
            for (const auto& field : declaration->second->fields) {
                if (field.name == operation.name) return field.is_weak;
            }
            std::string base;
            for (const auto& related : declaration->second->related_types) {
                if (related.category == IRValueCategory::ManagedReference) {
                    base = related.type.name;
                    break;
                }
            }
            owner = std::move(base);
        }
        return std::nullopt;
    }

    const IRCallableEffectSummary* call_summary(
        const IROperation& operation) const
    {
        if (operation.selected_declaration.empty()) return nullptr;
        if (!operation.substitutions.empty()) {
            const auto specialized = summaries_.find(
                specialization_identity(operation));
            if (specialized != summaries_.end()) return specialized->second;
        }
        const auto direct = summaries_.find(operation.selected_declaration);
        return direct == summaries_.end() ? nullptr : direct->second;
    }

    void classify_call(
        const IROperation& operation,
        const AllocationSet& arguments)
    {
        if (arguments.empty() || operation.name == "print") return;
        if (operation.kind == IROperationKind::ForeignCall) {
            add_cause(arguments, IREscapeReason::ForeignCall, operation.origin,
                operation.selected_declaration.empty()
                    ? operation.name : operation.selected_declaration,
                true, true);
            return;
        }
        if (operation.kind == IROperationKind::VirtualCall
            || operation.kind == IROperationKind::InterfaceCall) {
            add_cause(arguments, IREscapeReason::DynamicCall, operation.origin,
                operation.selected_declaration, true, true);
            return;
        }
        if (operation.kind == IROperationKind::Conversion
            && operation.selected_declaration.empty()) {
            return;
        }
        const auto* summary = call_summary(operation);
        if (!summary) {
            add_cause(arguments, IREscapeReason::UnknownCall, operation.origin,
                operation.selected_declaration.empty()
                    ? operation.name : operation.selected_declaration,
                true, true);
            return;
        }
        if (has_effect(*summary, IREffect::Escape)
            || has_effect(*summary, IREffect::ForeignCall)
            || has_effect(*summary, IREffect::UnknownExternal)
            || has_effect(*summary, IREffect::DynamicDispatch)) {
            add_cause(arguments, IREscapeReason::CallArgument, operation.origin,
                summary->callable_identity, true, true);
        }
    }

    AllocationSet evaluate(
        const IROperation& operation,
        AliasState& aliases,
        std::string_view callable)
    {
        if (operation.kind == IROperationKind::Load) {
            const auto found = aliases.values.find(operation.name);
            return found == aliases.values.end() ? AllocationSet{} : found->second;
        }

        AllocationSet children;
        for (const auto& child : operation.children) {
            merge_into(children, evaluate(child, aliases, callable));
        }

        if (operation.kind == IROperationKind::Construction
            && operation.result_type
            && is_managed_reference(*operation.result_type)) {
            if (!children.empty()) {
                add_cause(children, IREscapeReason::CallArgument, operation.origin,
                    operation.selected_declaration, true, true);
            }
            return AllocationSet{add_allocation(operation, callable)};
        }

        switch (operation.kind) {
        case IROperationKind::Call:
        case IROperationKind::ForeignCall:
        case IROperationKind::VirtualCall:
        case IROperationKind::InterfaceCall:
        case IROperationKind::Conversion:
            if (operation.kind == IROperationKind::ForeignCall) {
                for (const auto& [name, allocations] : aliases.values) {
                    static_cast<void>(name);
                    merge_into(children, allocations);
                }
            }
            classify_call(operation, children);
            if (operation.result_type
                && (is_reference_wrapper(*operation.result_type)
                    || operation.result_type->category
                        == IRValueCategory::ResultValue)) {
                return children;
            }
            return {};
        case IROperationKind::CollectionOperation:
            if (operation.name == "Array" || operation.name == "List"
                || operation.name == "Map" || operation.name == "add"
                || operation.name == "append" || operation.name == "insert") {
                add_cause(children, IREscapeReason::CollectionStorage,
                    operation.origin, operation.name, true);
            }
            return {};
        case IROperationKind::EnumConstruction:
            add_cause(children, IREscapeReason::EnumPayload, operation.origin,
                operation.name, false);
            return children;
        case IROperationKind::ResultConstruction:
            add_cause(children, IREscapeReason::ResultPayload, operation.origin,
                operation.name, false);
            return children;
        case IROperationKind::PropagateResult:
            return children;
        default:
            break;
        }
        if (operation.result_type && is_reference_wrapper(*operation.result_type)) {
            return children;
        }
        return {};
    }

    void merge_existing(
        AliasState& destination,
        const AliasState& first,
        const AliasState& second) const
    {
        for (auto& [name, allocations] : destination.values) {
            allocations.clear();
            if (const auto found = first.values.find(name);
                found != first.values.end()) {
                merge_into(allocations, found->second);
            }
            if (const auto found = second.values.find(name);
                found != second.values.end()) {
                merge_into(allocations, found->second);
            }
        }
    }

    void analyze_scoped_block(
        const IROperation& block,
        AliasState& aliases,
        std::string_view callable)
    {
        AliasState nested = aliases;
        analyze_block(block, nested, callable);
        const AliasState original = aliases;
        merge_existing(aliases, original, nested);
    }

    void analyze_branch(
        const IROperation& operation,
        AliasState& aliases,
        std::string_view callable)
    {
        if (!operation.children.empty()) {
            static_cast<void>(evaluate(operation.children.front(), aliases, callable));
        }
        AliasState then_state = aliases;
        AliasState else_state = aliases;
        if (operation.children.size() > 1) {
            analyze_statement(operation.children[1], then_state, callable);
        }
        if (operation.children.size() > 2) {
            analyze_statement(operation.children[2], else_state, callable);
        }
        merge_existing(aliases, then_state, else_state);
    }

    void analyze_loop(
        const IROperation& operation,
        AliasState& aliases,
        std::string_view callable)
    {
        if (!operation.children.empty()) {
            static_cast<void>(evaluate(operation.children.front(), aliases, callable));
        }
        const AliasState original = aliases;
        AliasState fixed = aliases;
        while (true) {
            AliasState body = fixed;
            if (operation.children.size() > 1) {
                analyze_statement(operation.children[1], body, callable);
            }
            AliasState next = original;
            merge_existing(next, fixed, body);
            if (next.values == fixed.values) break;
            fixed = std::move(next);
        }
        aliases = std::move(fixed);
    }

    void analyze_handle(
        const IROperation& operation,
        AliasState& aliases,
        std::string_view callable)
    {
        AllocationSet handled;
        if (!operation.children.empty()) {
            handled = evaluate(operation.children.front(), aliases, callable);
        }
        AliasState merged = aliases;
        for (std::size_t index = 1; index < operation.children.size(); ++index) {
            AliasState branch = aliases;
            const auto& block = operation.children[index];
            const auto open = block.name.find('(');
            const auto close = block.name.rfind(')');
            if (open != std::string::npos && close > open + 1) {
                branch.values.insert_or_assign(
                    block.name.substr(open + 1, close - open - 1), handled);
            }
            analyze_block(block, branch, callable);
            const AliasState prior = merged;
            merge_existing(merged, prior, branch);
        }
        aliases = std::move(merged);
    }

    void analyze_statement(
        const IROperation& operation,
        AliasState& aliases,
        std::string_view callable)
    {
        switch (operation.kind) {
        case IROperationKind::Declare:
        case IROperationKind::Assign:
            if (!operation.children.empty()) {
                auto value = evaluate(operation.children.back(), aliases, callable);
                if (operation.result_type
                    && operation.result_type->category
                        == IRValueCategory::InterfaceValue
                    && !value.empty()) {
                    add_cause(value, IREscapeReason::InterfaceAlias,
                        operation.origin, operation.name, false);
                }
                aliases.values.insert_or_assign(operation.name, std::move(value));
            }
            return;
        case IROperationKind::Return:
            if (!operation.children.empty()) {
                const auto value = evaluate(
                    operation.children.front(), aliases, callable);
                add_cause(value, IREscapeReason::Return, operation.origin,
                    std::string(callable), true);
            }
            return;
        case IROperationKind::MemberAssign:
            if (operation.children.size() >= 2) {
                static_cast<void>(evaluate(
                    operation.children.front(), aliases, callable));
                const auto value = evaluate(
                    operation.children.back(), aliases, callable);
                const auto weak = weak_field(operation);
                if (weak && *weak) {
                    add_cause(value, IREscapeReason::WeakObservation,
                        operation.origin, operation.name, false);
                } else if (!weak) {
                    add_cause(value, IREscapeReason::UnknownCall,
                        operation.origin, "unresolved field " + operation.name,
                        true, true);
                } else {
                    add_cause(value, IREscapeReason::StrongField,
                        operation.origin, operation.name, true);
                }
            }
            return;
        case IROperationKind::IndexAssign:
            if (!operation.children.empty()) {
                for (std::size_t index = 0;
                     index + 1 < operation.children.size(); ++index) {
                    static_cast<void>(evaluate(
                        operation.children[index], aliases, callable));
                }
                const auto value = evaluate(
                    operation.children.back(), aliases, callable);
                add_cause(value, IREscapeReason::CollectionStorage,
                    operation.origin, "indexed assignment", true);
            }
            return;
        case IROperationKind::Block:
            analyze_scoped_block(operation, aliases, callable);
            return;
        case IROperationKind::Branch:
            analyze_branch(operation, aliases, callable);
            return;
        case IROperationKind::Loop:
        case IROperationKind::ForEach:
            analyze_loop(operation, aliases, callable);
            return;
        case IROperationKind::Handle:
            analyze_handle(operation, aliases, callable);
            return;
        default:
            static_cast<void>(evaluate(operation, aliases, callable));
            return;
        }
    }

    void analyze_block(
        const IROperation& block,
        AliasState& aliases,
        std::string_view callable)
    {
        for (const auto& operation : block.children) {
            analyze_statement(operation, aliases, callable);
        }
    }

    void publish_metrics()
    {
        program_.escape_metrics.managed_allocations
            = program_.escape_allocations.size();
        for (const auto& allocation : program_.escape_allocations) {
            switch (allocation.state) {
            case IREscapeState::DoesNotEscape:
                ++program_.escape_metrics.non_escaping;
                break;
            case IREscapeState::Escapes:
                ++program_.escape_metrics.escaping;
                break;
            case IREscapeState::Unknown:
                ++program_.escape_metrics.unknown;
                break;
            }
        }
    }

    IRProgram& program_;
    std::unordered_map<std::string, const IRDeclaration*> declarations_;
    std::unordered_map<std::string, const IRCallableEffectSummary*> summaries_;
    std::unordered_map<std::string, std::size_t> allocation_occurrences_;
    std::unordered_map<const IROperation*, std::size_t> allocation_sites_;
};

} // namespace

void EscapeAnalyzer::analyze(IRProgram& program) const
{
    Analysis(program).run();
}

} // namespace toro
