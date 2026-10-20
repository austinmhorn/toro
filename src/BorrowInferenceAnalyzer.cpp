#include "toro/BorrowInferenceAnalyzer.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace toro {
namespace {

constexpr std::size_t kReceiverIndex = std::numeric_limits<std::size_t>::max();

bool is_managed_reference(const IRType& type)
{
    return type.category == IRValueCategory::ManagedReference
        || type.category == IRValueCategory::InterfaceValue;
}

bool is_callable(const IRDeclaration& declaration)
{
    return declaration.kind == IRDeclarationKind::Function
        || declaration.kind == IRDeclarationKind::ForeignFunction
        || declaration.kind == IRDeclarationKind::Method
        || declaration.kind == IRDeclarationKind::Conversion;
}

std::string method_suffix(std::string_view identity)
{
    const auto separator = identity.rfind("::");
    return separator == std::string_view::npos
        ? std::string(identity) : std::string(identity.substr(separator + 2));
}

int use_rank(IRParameterUse use)
{
    switch (use) {
    case IRParameterUse::ObservedOnly: return 0;
    case IRParameterUse::RetainedOrEscaped: return 1;
    case IRParameterUse::Unknown: return 2;
    }
    return 2;
}

struct UseResult {
    IRParameterUse use{IRParameterUse::ObservedOnly};
    std::string reason{"all uses are operation-bounded observations"};
};

UseResult merge(UseResult left, UseResult right)
{
    return use_rank(right.use) > use_rank(left.use)
        ? std::move(right) : std::move(left);
}

class Analysis {
public:
    Analysis(IRProgram& program, bool enabled)
        : program_(program), enabled_(enabled)
    {
        for (auto& declaration : program_.declarations) {
            declarations_.emplace(declaration.identity, &declaration);
        }
    }

    void run()
    {
        program_.borrow_summaries.clear();
        clear_borrows();
        create_facts();
        if (enabled_) {
            stabilize_facts();
            while (apply_weak_callsite_barriers()) stabilize_facts();
            annotate_operations();
        }
        program_.arc_stage = IRArcStage::BorrowsAnalyzed;
    }

private:
    static std::string fact_key(
        std::string_view identity,
        std::size_t index)
    {
        return std::string(identity) + "#"
            + (index == kReceiverIndex ? "self" : std::to_string(index));
    }

    bool generic_reference_parameter(
        const IRDeclaration& declaration,
        const IRType& type) const
    {
        return std::ranges::any_of(
            declaration.generic_parameters,
            [&](const auto& parameter) {
                return parameter.name == type.type.name;
            });
    }

    bool class_receiver(const IRDeclaration& declaration) const
    {
        if (declaration.owner_identity.empty()) return false;
        const auto owner = declarations_.find(declaration.owner_identity);
        return owner != declarations_.end()
            && owner->second->kind == IRDeclarationKind::Class;
    }

    void add_fact(
        const IRDeclaration& declaration,
        std::string name,
        std::size_t index,
        IROrigin origin,
        bool receiver)
    {
        auto& summary = program_.borrow_summaries.emplace_back(
            IRParameterBorrowSummary{
                declaration.identity,
                std::move(name),
                index,
                enabled_ && declaration.body
                    ? IRParameterUse::ObservedOnly
                    : IRParameterUse::Unknown,
                std::move(origin),
                enabled_ && declaration.body
                    ? "all uses are operation-bounded observations"
                    : "callable body is unavailable",
                receiver,
            });
        facts_.emplace(fact_key(declaration.identity, index), &summary);
    }

    void create_facts()
    {
        std::size_t fact_count = 0;
        for (const auto& declaration : program_.declarations) {
            if (!is_callable(declaration)) continue;
            fact_count += declaration.parameters.size();
            if ((declaration.kind == IRDeclarationKind::Method
                    || declaration.kind == IRDeclarationKind::Conversion)
                && class_receiver(declaration)) {
                ++fact_count;
            }
        }
        program_.borrow_summaries.reserve(fact_count);
        for (const auto& declaration : program_.declarations) {
            if (!is_callable(declaration)) continue;
            if ((declaration.kind == IRDeclarationKind::Method
                    || declaration.kind == IRDeclarationKind::Conversion)
                && class_receiver(declaration)) {
                add_fact(declaration, "self", kReceiverIndex,
                    declaration.origin, true);
            }
            for (std::size_t index = 0;
                 index < declaration.parameters.size(); ++index) {
                const auto& parameter = declaration.parameters[index];
                if (!is_managed_reference(parameter.type)
                    && !generic_reference_parameter(
                        declaration, parameter.type)) {
                    continue;
                }
                add_fact(declaration, parameter.name, index,
                    parameter.origin, false);
            }
        }
    }

    IRParameterBorrowSummary* fact(
        std::string_view identity,
        std::size_t index) const
    {
        const auto found = facts_.find(fact_key(identity, index));
        return found == facts_.end() ? nullptr : found->second;
    }

    std::string class_base(std::string_view identity) const
    {
        const auto nominal = declarations_.find(std::string(identity));
        if (nominal == declarations_.end()
            || nominal->second->kind != IRDeclarationKind::Class) {
            return {};
        }
        for (const auto& related : nominal->second->related_types) {
            const auto candidate = declarations_.find(related.type.name);
            if (candidate != declarations_.end()
                && candidate->second->kind == IRDeclarationKind::Class) {
                return related.type.name;
            }
        }
        return {};
    }

    bool is_descendant(
        std::string_view candidate,
        std::string_view ancestor) const
    {
        std::string current(candidate);
        while (!current.empty()) {
            if (current == ancestor) return true;
            current = class_base(current);
        }
        return false;
    }

    bool implements_interface(
        std::string_view identity,
        std::string_view interface_identity) const
    {
        const auto nominal = declarations_.find(std::string(identity));
        if (nominal == declarations_.end()) return false;
        for (const auto& related : nominal->second->related_types) {
            if (related.type.name == interface_identity) return true;
        }
        const auto base = class_base(identity);
        return !base.empty()
            && implements_interface(base, interface_identity);
    }

    std::vector<const IRDeclaration*> call_candidates(
        const IROperation& operation) const
    {
        const auto selected = declarations_.find(operation.selected_declaration);
        if (selected == declarations_.end()) return {};
        if (operation.kind != IROperationKind::VirtualCall
            && operation.kind != IROperationKind::InterfaceCall) {
            return {selected->second};
        }
        const std::string owner = selected->second->owner_identity;
        const std::string suffix = method_suffix(operation.selected_declaration);
        std::vector<const IRDeclaration*> result;
        for (const auto& candidate : program_.declarations) {
            if (candidate.kind != IRDeclarationKind::Method || !candidate.body
                || candidate.name != operation.name
                || method_suffix(candidate.identity) != suffix) {
                continue;
            }
            const bool valid = operation.kind == IROperationKind::VirtualCall
                ? is_descendant(candidate.owner_identity, owner)
                : implements_interface(candidate.owner_identity, owner);
            if (valid) result.push_back(&candidate);
        }
        std::ranges::sort(result, {}, &IRDeclaration::identity);
        return result;
    }

    bool has_receiver(const IRDeclaration& declaration) const
    {
        return !declaration.owner_identity.empty()
            && (declaration.kind == IRDeclarationKind::Method
                || declaration.kind == IRDeclarationKind::Conversion);
    }

    std::vector<IRParameterBorrowSummary*> operand_facts(
        const IROperation& operation,
        std::size_t child_index) const
    {
        std::vector<IRParameterBorrowSummary*> result;
        for (const auto* candidate : call_candidates(operation)) {
            const bool receiver = has_receiver(*candidate);
            const std::size_t parameter_index = receiver
                ? (child_index == 0 ? kReceiverIndex : child_index - 1)
                : child_index;
            if (auto* value = fact(candidate->identity, parameter_index)) {
                result.push_back(value);
            } else {
                return {};
            }
        }
        return result;
    }

    UseResult classify_call_operand(
        const IROperation& call,
        std::size_t child_index) const
    {
        if (call.kind == IROperationKind::ForeignCall) {
            return {IRParameterUse::Unknown,
                "reference crosses a foreign-call boundary"};
        }
        const auto candidates = operand_facts(call, child_index);
        if (candidates.empty()) {
            return {IRParameterUse::Unknown,
                "callee ownership behavior is unavailable"};
        }
        UseResult result;
        for (const auto* candidate : candidates) {
            if (candidate->use == IRParameterUse::ObservedOnly) continue;
            result = merge(std::move(result), UseResult{
                candidate->use,
                "forwarded to " + candidate->callable_identity + "."
                    + candidate->parameter_name,
            });
        }
        return result;
    }

    UseResult classify_load_parent(
        const IROperation& parent,
        std::size_t child_index) const
    {
        switch (parent.kind) {
        case IROperationKind::MemberAccess:
            if (child_index == 0) return {};
            break;
        case IROperationKind::MemberAssign:
            if (child_index == 0) return {};
            return {IRParameterUse::RetainedOrEscaped,
                "stored in a strong or weak field"};
        case IROperationKind::IndexAccess:
            if (child_index == 0) return {};
            break;
        case IROperationKind::Binary:
            if (parent.name == "==" || parent.name == "!=") return {};
            break;
        case IROperationKind::Call:
        case IROperationKind::VirtualCall:
        case IROperationKind::InterfaceCall:
        case IROperationKind::ForeignCall:
            return classify_call_operand(parent, child_index);
        case IROperationKind::Return:
            return {IRParameterUse::RetainedOrEscaped,
                "returned from the callable"};
        case IROperationKind::Declare:
        case IROperationKind::Assign:
            return {IRParameterUse::RetainedOrEscaped,
                "stored in an owning local"};
        case IROperationKind::IndexAssign:
            return {IRParameterUse::RetainedOrEscaped,
                "stored in a collection"};
        case IROperationKind::CollectionOperation:
            return {IRParameterUse::RetainedOrEscaped,
                "passed to a collection operation"};
        case IROperationKind::EnumConstruction:
        case IROperationKind::ResultConstruction:
            return {IRParameterUse::RetainedOrEscaped,
                "stored in a managed payload"};
        default:
            break;
        }
        return {IRParameterUse::Unknown,
            "reference use is not proven operation-bounded"};
    }

    UseResult classify_name(
        const IROperation& operation,
        std::string_view name,
        const IROperation* parent = nullptr,
        std::size_t child_index = 0) const
    {
        UseResult result;
        if (operation.kind == IROperationKind::Load
            && operation.name == name) {
            return parent
                ? classify_load_parent(*parent, child_index)
                : UseResult{IRParameterUse::Unknown,
                    "reference load has no bounded consumer"};
        }
        for (std::size_t index = 0; index < operation.children.size(); ++index) {
            result = merge(std::move(result), classify_name(
                operation.children[index], name, &operation, index));
        }
        return result;
    }

    void stabilize_facts()
    {
        bool changed = true;
        while (changed) {
            changed = false;
            for (auto& summary : program_.borrow_summaries) {
                if (summary.use != IRParameterUse::ObservedOnly) continue;
                const auto declaration = declarations_.find(
                    summary.callable_identity);
                if (declaration == declarations_.end()
                    || !declaration->second->body) {
                    continue;
                }
                const auto result = classify_name(
                    *declaration->second->body, summary.parameter_name);
                if (result.use == IRParameterUse::ObservedOnly) continue;
                summary.use = result.use;
                summary.reason = result.reason;
                changed = true;
            }
        }
    }

    bool weak_field_access(const IROperation& operation) const
    {
        if (operation.kind != IROperationKind::MemberAccess
            || operation.children.empty()
            || !operation.children.front().result_type) {
            return false;
        }
        std::string owner = operation.children.front().result_type->type.name;
        while (!owner.empty()) {
            const auto declaration = declarations_.find(owner);
            if (declaration == declarations_.end()) return false;
            for (const auto& field : declaration->second->fields) {
                if (field.name == operation.name) return field.is_weak;
            }
            owner = class_base(owner);
        }
        return false;
    }

    bool apply_weak_barriers(IROperation& operation)
    {
        bool changed = false;
        if (operation.kind == IROperationKind::Call
            || operation.kind == IROperationKind::VirtualCall
            || operation.kind == IROperationKind::InterfaceCall) {
            for (std::size_t index = 0;
                 index < operation.children.size(); ++index) {
                if (!weak_field_access(operation.children[index])) continue;
                for (auto* summary : operand_facts(operation, index)) {
                    if (summary->use != IRParameterUse::ObservedOnly) continue;
                    summary->use = IRParameterUse::Unknown;
                    summary->reason =
                        "weak-reference arguments retain their existing strong-call guard";
                    changed = true;
                }
            }
        }
        for (auto& child : operation.children) {
            changed = apply_weak_barriers(child) || changed;
        }
        return changed;
    }

    bool apply_weak_callsite_barriers()
    {
        bool changed = false;
        for (auto& declaration : program_.declarations) {
            if (declaration.body) {
                changed = apply_weak_barriers(*declaration.body) || changed;
            }
        }
        for (auto& operation : program_.top_level_operations) {
            changed = apply_weak_barriers(operation) || changed;
        }
        return changed;
    }

    static std::string source_name(const IROperation& operation)
    {
        if (operation.kind == IROperationKind::Load) return operation.name;
        if (operation.kind == IROperationKind::MemberAccess) {
            return "member " + operation.name;
        }
        if (operation.kind == IROperationKind::IndexAccess) {
            return "collection element";
        }
        return "temporary";
    }

    void annotate_call(IROperation& operation)
    {
        if (operation.kind != IROperationKind::Call
            && operation.kind != IROperationKind::VirtualCall
            && operation.kind != IROperationKind::InterfaceCall) {
            return;
        }
        const auto candidates = call_candidates(operation);
        if (candidates.empty()) return;
        const bool receiver = has_receiver(*candidates.front());
        for (std::size_t index = 0;
             index < operation.children.size(); ++index) {
            const auto& child = operation.children[index];
            if (!child.result_type || !is_managed_reference(*child.result_type)
                || weak_field_access(child)) {
                continue;
            }
            const auto summaries = operand_facts(operation, index);
            if (summaries.empty()
                || !std::ranges::all_of(summaries, [](const auto* summary) {
                    return summary->use == IRParameterUse::ObservedOnly;
                })) {
                continue;
            }
            const bool is_receiver = receiver && index == 0;
            const std::size_t parameter_index = is_receiver
                ? 0 : (receiver ? index - 1 : index);
            const std::string destination = is_receiver
                ? operation.name + ".self"
                : operation.name + "."
                    + (parameter_index < candidates.front()->parameters.size()
                        ? candidates.front()->parameters[parameter_index].name
                        : std::to_string(parameter_index));
            operation.borrows.push_back(IRBorrowUse{
                is_receiver ? IRBorrowKind::Receiver
                            : IRBorrowKind::CallArgument,
                index,
                source_name(child),
                destination,
                operation.origin,
                operation.kind == IROperationKind::VirtualCall
                        || operation.kind == IROperationKind::InterfaceCall
                    ? "all reachable dispatch targets observe without retaining"
                    : "selected declaration observes without retaining",
            });
        }
    }

    bool operation_uses_name(
        const IROperation& operation,
        std::string_view name) const
    {
        if (operation.kind == IROperationKind::Load
            && operation.name == name) {
            return true;
        }
        return std::ranges::any_of(operation.children,
            [&](const auto& child) {
                return operation_uses_name(child, name);
            });
    }

    bool alias_use_is_observing(
        const IROperation& operation,
        std::string_view alias,
        const IROperation* parent = nullptr,
        std::size_t child_index = 0,
        bool uncertain_control = false) const
    {
        if (operation.kind == IROperationKind::Load
            && operation.name == alias) {
            if (uncertain_control || !parent) return false;
            return classify_load_parent(*parent, child_index).use
                == IRParameterUse::ObservedOnly;
        }
        const bool nested_uncertainty = uncertain_control
            || operation.kind == IROperationKind::Branch
            || operation.kind == IROperationKind::Loop
            || operation.kind == IROperationKind::ForEach
            || operation.kind == IROperationKind::Handle;
        for (std::size_t index = 0; index < operation.children.size(); ++index) {
            if (!alias_use_is_observing(operation.children[index], alias,
                    &operation, index, nested_uncertainty)) {
                return false;
            }
        }
        return true;
    }

    bool invalidates_owner_or_alias(
        const IROperation& operation,
        std::string_view owner,
        std::string_view alias) const
    {
        if ((operation.kind == IROperationKind::Assign
                && (operation.name == owner || operation.name == alias))
            || (operation.ownership_transfer
                && operation.ownership_transfer->source == owner)) {
            return true;
        }
        return std::ranges::any_of(operation.children,
            [&](const auto& child) {
                return invalidates_owner_or_alias(child, owner, alias);
            });
    }

    void annotate_local_aliases(
        IROperation& block,
        std::unordered_set<std::string> available)
    {
        if (block.kind != IROperationKind::Block) return;
        for (std::size_t index = 0; index < block.children.size(); ++index) {
            auto& operation = block.children[index];
            if (operation.kind == IROperationKind::Declare
                && operation.children.size() == 1
                && operation.children.front().kind == IROperationKind::Load
                && operation.result_type
                && is_managed_reference(*operation.result_type)
                && !operation.ownership_transfer) {
                const std::string& owner = operation.children.front().name;
                bool valid = available.contains(owner);
                std::size_t last_use = index;
                for (std::size_t later = index + 1;
                     later < block.children.size(); ++later) {
                    if (operation_uses_name(
                            block.children[later], operation.name)) {
                        last_use = later;
                    }
                }
                for (std::size_t later = index + 1;
                     valid && later <= last_use; ++later) {
                    const auto& use = block.children[later];
                    if (invalidates_owner_or_alias(
                            use, owner, operation.name)) {
                        valid = false;
                    } else if (operation_uses_name(use, operation.name)
                        && !alias_use_is_observing(
                            use, operation.name)) {
                        valid = false;
                    }
                }
                if (valid) {
                    operation.borrows.push_back(IRBorrowUse{
                        IRBorrowKind::LocalAlias,
                        0,
                        owner,
                        operation.name,
                        operation.origin,
                        "owner remains live for the lexical alias lifetime",
                    });
                }
            }
            if (operation.kind == IROperationKind::Declare
                && operation.result_type
                && is_managed_reference(*operation.result_type)) {
                available.insert(operation.name);
            }
            for (auto& child : operation.children) {
                if (child.kind == IROperationKind::Block) {
                    annotate_local_aliases(child, available);
                }
            }
        }
    }

    void annotate_operation(IROperation& operation)
    {
        annotate_call(operation);
        for (auto& child : operation.children) annotate_operation(child);
    }

    void annotate_operations()
    {
        for (auto& declaration : program_.declarations) {
            if (!declaration.body) continue;
            annotate_operation(*declaration.body);
            std::unordered_set<std::string> available;
            for (const auto& parameter : declaration.parameters) {
                if (is_managed_reference(parameter.type)
                    || generic_reference_parameter(
                        declaration, parameter.type)) {
                    available.insert(parameter.name);
                }
            }
            if (class_receiver(declaration)) available.insert("self");
            annotate_local_aliases(*declaration.body, std::move(available));
        }
        for (auto& operation : program_.top_level_operations) {
            annotate_operation(operation);
        }
    }

    static void clear_operation_borrows(IROperation& operation)
    {
        operation.borrows.clear();
        for (auto& child : operation.children) clear_operation_borrows(child);
    }

    void clear_borrows()
    {
        for (auto& declaration : program_.declarations) {
            if (declaration.body) clear_operation_borrows(*declaration.body);
        }
        for (auto& operation : program_.top_level_operations) {
            clear_operation_borrows(operation);
        }
    }

    IRProgram& program_;
    bool enabled_;
    std::unordered_map<std::string, IRDeclaration*> declarations_;
    std::unordered_map<std::string, IRParameterBorrowSummary*> facts_;
};

} // namespace

void BorrowInferenceAnalyzer::analyze(
    IRProgram& program,
    bool enable_borrowing) const
{
    if (program.arc_stage != IRArcStage::TransfersAnalyzed) {
        throw std::runtime_error(
            "borrow inference requires completed ownership-transfer analysis");
    }
    Analysis(program, enable_borrowing).run();
}

} // namespace toro
