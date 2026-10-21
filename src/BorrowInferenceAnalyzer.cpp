#include "toro/BorrowInferenceAnalyzer.hpp"

#include "toro/OwnershipSummaryAnalyzer.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace toro {
namespace {

bool is_reference(const IRType& type)
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

class Analysis {
public:
    Analysis(IRProgram& program, bool enabled)
        : program_(program), enabled_(enabled)
    {
        for (const auto& declaration : program_.declarations) {
            declarations_.emplace(declaration.identity, &declaration);
        }
    }

    void run()
    {
        clear_borrows();
        program_.borrowing_enabled = enabled_;
        if (enabled_) annotate_operations();
        program_.arc_stage = IRArcStage::BorrowsAnalyzed;
    }

private:
    std::string class_base(std::string_view identity) const
    {
        const auto nominal = declarations_.find(std::string(identity));
        if (nominal == declarations_.end()
            || nominal->second->kind != IRDeclarationKind::Class) return {};
        for (const auto& related : nominal->second->related_types) {
            const auto candidate = declarations_.find(related.type.name);
            if (candidate != declarations_.end()
                && candidate->second->kind == IRDeclarationKind::Class) {
                return related.type.name;
            }
        }
        return {};
    }

    bool class_receiver(const IRDeclaration& declaration) const
    {
        const auto owner = declarations_.find(declaration.owner_identity);
        return owner != declarations_.end()
            && owner->second->kind == IRDeclarationKind::Class;
    }

    bool generic_reference_parameter(
        const IRDeclaration& declaration,
        const IRType& type) const
    {
        return std::ranges::any_of(declaration.generic_parameters,
            [&](const auto& parameter) {
                return parameter.name == type.type.name;
            });
    }

    bool weak_field_access(const IROperation& operation) const
    {
        if (operation.kind != IROperationKind::MemberAccess
            || operation.children.empty()
            || !operation.children.front().result_type) return false;
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
            && operation.kind != IROperationKind::InterfaceCall
            && operation.kind != IROperationKind::Conversion) return;
        for (std::size_t index = 0;
             index < operation.children.size(); ++index) {
            const auto& child = operation.children[index];
            if (!child.result_type || !is_reference(*child.result_type)
                || weak_field_access(child)) continue;
            const auto summary = ownership_for_call_operand(
                program_, operation, index);
            if (!summary || !ownership_is_observed_only(*summary)) continue;
            const bool receiver = summary->receiver;
            operation.borrows.push_back(IRBorrowUse{
                receiver ? IRBorrowKind::Receiver
                         : IRBorrowKind::CallArgument,
                index,
                source_name(child),
                receiver ? operation.name + ".self"
                         : operation.name + "." + summary->parameter_name,
                operation.origin,
                operation.kind == IROperationKind::VirtualCall
                        || operation.kind == IROperationKind::InterfaceCall
                    ? "all reachable ownership summaries are observation-only"
                    : "selected ownership summary is observation-only",
            });
        }
    }

    bool operation_uses_name(
        const IROperation& operation,
        std::string_view name) const
    {
        if (operation.kind == IROperationKind::Load
            && operation.name == name) return true;
        return std::ranges::any_of(operation.children,
            [&](const auto& child) {
                return operation_uses_name(child, name);
            });
    }

    bool load_is_observing(
        const IROperation& parent,
        std::size_t child_index) const
    {
        switch (parent.kind) {
        case IROperationKind::MemberAccess:
        case IROperationKind::IndexAccess:
        case IROperationKind::MemberAssign:
        case IROperationKind::IndexAssign:
            return child_index == 0;
        case IROperationKind::Binary:
            return parent.name == "==" || parent.name == "!=";
        case IROperationKind::Call:
        case IROperationKind::VirtualCall:
        case IROperationKind::InterfaceCall:
        case IROperationKind::Conversion: {
            const auto summary = ownership_for_call_operand(
                program_, parent, child_index);
            return summary && ownership_is_observed_only(*summary);
        }
        default:
            return false;
        }
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
            return !uncertain_control && parent
                && load_is_observing(*parent, child_index);
        }
        const bool nested_uncertainty = uncertain_control
            || operation.kind == IROperationKind::Branch
            || operation.kind == IROperationKind::Loop
            || operation.kind == IROperationKind::ForEach
            || operation.kind == IROperationKind::Handle;
        for (std::size_t index = 0; index < operation.children.size(); ++index) {
            if (!alias_use_is_observing(operation.children[index], alias,
                    &operation, index, nested_uncertainty)) return false;
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
                && operation.ownership_transfer->source == owner)) return true;
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
            const bool class_backed_reference = operation.result_type
                && (operation.result_type->category
                        == IRValueCategory::ManagedReference
                    || (operation.result_type->category
                            == IRValueCategory::InterfaceValue
                        && operation.children.size() == 1
                        && operation.children.front().result_type
                        && operation.children.front().result_type->category
                            == IRValueCategory::ManagedReference));
            if (operation.kind == IROperationKind::Declare
                && operation.children.size() == 1
                && operation.children.front().kind == IROperationKind::Load
                && class_backed_reference
                && !operation.ownership_transfer) {
                const std::string& owner = operation.children.front().name;
                bool valid = available.contains(owner);
                std::size_t last_use = index;
                for (std::size_t later = index + 1;
                     later < block.children.size(); ++later) {
                    if (operation_uses_name(block.children[later], operation.name)) {
                        last_use = later;
                    }
                }
                for (std::size_t later = index + 1;
                     valid && later < block.children.size(); ++later) {
                    const auto& use = block.children[later];
                    const bool invalidated = later <= last_use
                        ? invalidates_owner_or_alias(
                            use, owner, operation.name)
                        : use.kind == IROperationKind::Assign
                            && use.name == operation.name;
                    if (invalidated
                        || (later <= last_use
                            && operation_uses_name(use, operation.name)
                            && !alias_use_is_observing(use, operation.name))) {
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
                && class_backed_reference) {
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
                if (is_reference(parameter.type)
                    || generic_reference_parameter(declaration, parameter.type)) {
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
    std::unordered_map<std::string, const IRDeclaration*> declarations_;
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
    if (std::ranges::any_of(program.declarations, is_callable)
        && program.ownership_summaries.empty()) {
        throw std::runtime_error(
            "borrow inference requires ownership summaries");
    }
    Analysis(program, enable_borrowing).run();
}

} // namespace toro
