#include "toro/OwnershipSummaryAnalyzer.hpp"

#include "toro/StandardLibrary.hpp"

#include <algorithm>
#include <array>
#include <bitset>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace toro {
namespace {

constexpr std::size_t kReceiverIndex = std::numeric_limits<std::size_t>::max();
constexpr std::size_t kBehaviorCount = 8;
using BehaviorBits = std::bitset<kBehaviorCount>;

constexpr std::array behavior_order{
    IROwnershipBehavior::Observe,
    IROwnershipBehavior::Retain,
    IROwnershipBehavior::Store,
    IROwnershipBehavior::Return,
    IROwnershipBehavior::Consume,
    IROwnershipBehavior::Escape,
    IROwnershipBehavior::WeakStore,
    IROwnershipBehavior::Unknown,
};

constexpr std::size_t behavior_index(IROwnershipBehavior behavior)
{
    return static_cast<std::size_t>(behavior);
}

bool is_callable(const IRDeclaration& declaration)
{
    return declaration.kind == IRDeclarationKind::Function
        || declaration.kind == IRDeclarationKind::ForeignFunction
        || declaration.kind == IRDeclarationKind::Method
        || declaration.kind == IRDeclarationKind::Conversion;
}

bool is_reference(const IRType& type)
{
    return type.category == IRValueCategory::ManagedReference
        || type.category == IRValueCategory::InterfaceValue;
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

std::string specialization_identity(
    const IROperation& operation,
    const std::vector<std::pair<std::string, IRType>>& caller = {})
{
    std::string result = operation.selected_declaration;
    if (operation.substitutions.empty()) return result;
    result += '<';
    for (std::size_t index = 0; index < operation.substitutions.size(); ++index) {
        if (index != 0) result += ',';
        const auto& concrete = substitute_type(
            operation.substitutions[index].second, caller);
        result += operation.substitutions[index].first + '='
            + type_key(concrete.type);
    }
    result += '>';
    return result;
}

std::string method_suffix(std::string_view identity)
{
    const auto separator = identity.rfind("::");
    return separator == std::string_view::npos
        ? std::string(identity) : std::string(identity.substr(separator + 2));
}

bool has_behavior(
    const IRParameterOwnershipSummary& summary,
    IROwnershipBehavior behavior)
{
    return std::ranges::find(summary.behaviors, behavior)
        != summary.behaviors.end();
}

void add_behavior(
    IRParameterOwnershipSummary& destination,
    IROwnershipBehavior behavior,
    const IROwnershipCause* cause = nullptr)
{
    if (!has_behavior(destination, behavior)) {
        destination.behaviors.push_back(behavior);
    }
    if (!cause) return;
    const bool duplicate = std::ranges::any_of(
        destination.causes,
        [&](const auto& existing) {
            return existing.behavior == behavior
                && existing.origin.module_name == cause->origin.module_name
                && existing.origin.location.line == cause->origin.location.line
                && existing.origin.location.column == cause->origin.location.column
                && existing.via_callable == cause->via_callable;
        });
    if (!duplicate) destination.causes.push_back(*cause);
}

void normalize(IRParameterOwnershipSummary& summary)
{
    if (summary.behaviors.empty()) {
        summary.behaviors.push_back(IROwnershipBehavior::Observe);
    } else if (summary.behaviors.size() > 1) {
        std::erase(summary.behaviors, IROwnershipBehavior::Observe);
    }
    std::ranges::sort(summary.behaviors, {}, [](auto behavior) {
        return behavior_index(behavior);
    });
    std::ranges::sort(summary.causes, [](const auto& left, const auto& right) {
        if (left.behavior != right.behavior) {
            return behavior_index(left.behavior) < behavior_index(right.behavior);
        }
        if (left.origin.module_name != right.origin.module_name) {
            return left.origin.module_name < right.origin.module_name;
        }
        if (left.origin.location.line != right.origin.location.line) {
            return left.origin.location.line < right.origin.location.line;
        }
        if (left.origin.location.column != right.origin.location.column) {
            return left.origin.location.column < right.origin.location.column;
        }
        return left.via_callable < right.via_callable;
    });
}

class Resolver {
public:
    explicit Resolver(const IRProgram& program) : program_(program)
    {
        for (const auto& declaration : program.declarations) {
            declarations_.emplace(declaration.identity, &declaration);
            if (declaration.kind == IRDeclarationKind::Class
                || declaration.kind == IRDeclarationKind::Struct
                || declaration.kind == IRDeclarationKind::Interface) {
                nominals_.emplace(declaration.identity, &declaration);
            }
        }
        for (const auto& summary : program.ownership_summaries) {
            summaries_.emplace(summary.callable_identity, &summary);
        }
    }

    std::string class_base(std::string_view identity) const
    {
        const auto nominal = nominals_.find(std::string(identity));
        if (nominal == nominals_.end()
            || nominal->second->kind != IRDeclarationKind::Class) {
            return {};
        }
        for (const auto& related : nominal->second->related_types) {
            const auto candidate = nominals_.find(related.type.name);
            if (candidate != nominals_.end()
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
        if (candidate.empty() || ancestor.empty()) return false;
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
        const auto nominal = nominals_.find(std::string(identity));
        if (nominal == nominals_.end()) return false;
        for (const auto& related : nominal->second->related_types) {
            if (related.type.name == interface_identity) return true;
        }
        const auto base = class_base(identity);
        return !base.empty() && implements_interface(base, interface_identity);
    }

    std::vector<const IRDeclaration*> candidates(
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
            bool valid = operation.kind == IROperationKind::VirtualCall
                ? is_descendant(candidate.owner_identity, owner) : false;
            if (operation.kind == IROperationKind::InterfaceCall) {
                for (const auto& [identity, nominal] : nominals_) {
                    if ((nominal->kind == IRDeclarationKind::Class
                            || nominal->kind == IRDeclarationKind::Struct)
                        && implements_interface(identity, owner)
                        && is_descendant(identity, candidate.owner_identity)) {
                        valid = true;
                        break;
                    }
                }
            }
            if (valid) result.push_back(&candidate);
        }
        std::ranges::sort(result, {}, &IRDeclaration::identity);
        return result;
    }

    const IRCallableOwnershipSummary* summary_for(
        const IROperation& operation,
        const IRDeclaration& declaration) const
    {
        if (!operation.substitutions.empty()
            && declaration.identity == operation.selected_declaration) {
            const auto concrete = summaries_.find(
                specialization_identity(operation));
            if (concrete != summaries_.end()) return concrete->second;
        }
        const auto found = summaries_.find(declaration.identity);
        return found == summaries_.end() ? nullptr : found->second;
    }

private:
    const IRProgram& program_;
    std::unordered_map<std::string, const IRDeclaration*> declarations_;
    std::unordered_map<std::string, const IRDeclaration*> nominals_;
    std::unordered_map<std::string, const IRCallableOwnershipSummary*> summaries_;
};

struct ParameterState {
    std::string name;
    std::size_t index{0};
    IROrigin origin;
    bool receiver{false};
    BehaviorBits behaviors;
    std::array<std::optional<IROwnershipCause>, kBehaviorCount> causes;
};

struct Node {
    std::string identity;
    const IRDeclaration* declaration{nullptr};
    std::vector<std::pair<std::string, IRType>> substitutions;
    std::vector<ParameterState> parameters;
    IRReturnOwnershipSummary return_value;
};

struct UseResult {
    BehaviorBits behaviors;
    std::array<std::optional<IROwnershipCause>, kBehaviorCount> causes;
};

void merge_into(UseResult& destination, const UseResult& source)
{
    destination.behaviors |= source.behaviors;
    for (std::size_t index = 0; index < kBehaviorCount; ++index) {
        if (!destination.causes[index] && source.causes[index]) {
            destination.causes[index] = source.causes[index];
        }
    }
}

UseResult behavior(
    IROwnershipBehavior value,
    const IROrigin& origin,
    std::string reason,
    std::string via = {},
    bool transitive = false)
{
    UseResult result;
    const auto index = behavior_index(value);
    result.behaviors.set(index);
    result.causes[index] = IROwnershipCause{
        value, origin, std::move(via), std::move(reason), transitive};
    return result;
}

class Analysis {
public:
    explicit Analysis(IRProgram& program) : program_(program)
    {
        for (const auto& declaration : program_.declarations) {
            declarations_.emplace(declaration.identity, &declaration);
            if (declaration.kind == IRDeclarationKind::Class
                || declaration.kind == IRDeclarationKind::Struct
                || declaration.kind == IRDeclarationKind::Interface) {
                nominals_.emplace(declaration.identity, &declaration);
            }
        }
    }

    void run()
    {
        program_.ownership_summaries.clear();
        collect_nodes();
        initialize_parameters();
        stabilize();
        publish();
    }

private:
    bool generic_parameter(
        const IRDeclaration& declaration,
        const IRType& type) const
    {
        return std::ranges::any_of(declaration.generic_parameters,
            [&](const auto& parameter) {
                return parameter.name == type.type.name;
            });
    }

    bool class_receiver(const IRDeclaration& declaration) const
    {
        const auto owner = declarations_.find(declaration.owner_identity);
        return owner != declarations_.end()
            && owner->second->kind == IRDeclarationKind::Class;
    }

    void add_node(
        std::string identity,
        const IRDeclaration& declaration,
        std::vector<std::pair<std::string, IRType>> substitutions)
    {
        if (indices_.contains(identity)) return;
        indices_.emplace(identity, nodes_.size());
        nodes_.push_back(Node{
            std::move(identity), &declaration, std::move(substitutions), {}, {}});
    }

    void collect_specializations(const IROperation& operation)
    {
        if (!operation.selected_declaration.empty()
            && !operation.substitutions.empty()) {
            if (const auto found = declarations_.find(operation.selected_declaration);
                found != declarations_.end()) {
                add_node(specialization_identity(operation), *found->second,
                    operation.substitutions);
            }
        }
        for (const auto& child : operation.children) collect_specializations(child);
    }

    void collect_nodes()
    {
        for (const auto& declaration : program_.declarations) {
            if (is_callable(declaration)) add_node(declaration.identity, declaration, {});
        }
        for (const auto& declaration : program_.declarations) {
            if (declaration.body) collect_specializations(*declaration.body);
            for (const auto& field : declaration.fields) {
                if (field.default_value) collect_specializations(*field.default_value);
            }
        }
        for (const auto& operation : program_.top_level_operations) {
            collect_specializations(operation);
        }
    }

    const IRType& concrete(const IRType& type, const Node& node) const
    {
        return substitute_type(type, node.substitutions);
    }

    void initialize_parameters()
    {
        for (auto& node : nodes_) {
            const auto& declaration = *node.declaration;
            if ((declaration.kind == IRDeclarationKind::Method
                    || declaration.kind == IRDeclarationKind::Conversion)
                && class_receiver(declaration)) {
                node.parameters.push_back(ParameterState{
                    "self", kReceiverIndex, declaration.origin, true, {}, {}});
            }
            for (std::size_t index = 0;
                 index < declaration.parameters.size(); ++index) {
                const auto& parameter = declaration.parameters[index];
                const auto& type = concrete(parameter.type, node);
                if (!is_reference(type)
                    && !(node.substitutions.empty()
                        && generic_parameter(declaration, parameter.type))) {
                    continue;
                }
                node.parameters.push_back(ParameterState{
                    parameter.name, index, parameter.origin, false, {}, {}});
            }
            if (declaration.kind == IRDeclarationKind::ForeignFunction
                || !declaration.body) {
                for (auto& parameter : node.parameters) {
                    const auto result = behavior(IROwnershipBehavior::Unknown,
                        declaration.origin,
                        declaration.kind == IRDeclarationKind::ForeignFunction
                            ? "foreign ownership behavior is unavailable"
                            : "callable body is unavailable");
                    parameter.behaviors = result.behaviors;
                    parameter.causes = result.causes;
                }
            }
        }
    }

    Node* node_for(
        const IROperation& operation,
        const Node& caller,
        const IRDeclaration& declaration)
    {
        if (!operation.substitutions.empty()
            && declaration.identity == operation.selected_declaration) {
            const auto concrete_identity = specialization_identity(
                operation, caller.substitutions);
            if (const auto found = indices_.find(concrete_identity);
                found != indices_.end()) {
                return &nodes_[found->second];
            }
            const auto symbolic = specialization_identity(operation);
            if (const auto found = indices_.find(symbolic);
                found != indices_.end()) {
                return &nodes_[found->second];
            }
        }
        const auto found = indices_.find(declaration.identity);
        return found == indices_.end() ? nullptr : &nodes_[found->second];
    }

    std::string class_base(std::string_view identity) const
    {
        const auto nominal = nominals_.find(std::string(identity));
        if (nominal == nominals_.end()
            || nominal->second->kind != IRDeclarationKind::Class) return {};
        for (const auto& related : nominal->second->related_types) {
            const auto candidate = nominals_.find(related.type.name);
            if (candidate != nominals_.end()
                && candidate->second->kind == IRDeclarationKind::Class) {
                return related.type.name;
            }
        }
        return {};
    }

    bool is_descendant(std::string_view candidate, std::string_view ancestor) const
    {
        if (candidate.empty() || ancestor.empty()) return false;
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
        const auto nominal = nominals_.find(std::string(identity));
        if (nominal == nominals_.end()) return false;
        for (const auto& related : nominal->second->related_types) {
            if (related.type.name == interface_identity) return true;
        }
        const auto base = class_base(identity);
        return !base.empty() && implements_interface(base, interface_identity);
    }

    std::vector<const IRDeclaration*> candidates(const IROperation& operation) const
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
                || method_suffix(candidate.identity) != suffix) continue;
            bool valid = operation.kind == IROperationKind::VirtualCall
                ? is_descendant(candidate.owner_identity, owner) : false;
            if (operation.kind == IROperationKind::InterfaceCall) {
                for (const auto& [identity, nominal] : nominals_) {
                    if ((nominal->kind == IRDeclarationKind::Class
                            || nominal->kind == IRDeclarationKind::Struct)
                        && implements_interface(identity, owner)
                        && is_descendant(identity, candidate.owner_identity)) {
                        valid = true;
                        break;
                    }
                }
            }
            if (valid) result.push_back(&candidate);
        }
        std::ranges::sort(result, {}, &IRDeclaration::identity);
        return result;
    }

    ParameterState* parameter_for(
        Node& node,
        std::size_t child_index,
        bool has_receiver)
    {
        const std::size_t parameter_index = has_receiver
            ? (child_index == 0 ? kReceiverIndex : child_index - 1)
            : child_index;
        const auto found = std::ranges::find_if(node.parameters,
            [&](const auto& parameter) {
                return parameter.index == parameter_index;
            });
        return found == node.parameters.end() ? nullptr : &*found;
    }

    UseResult classify_call(
        const IROperation& call,
        std::size_t child_index,
        const Node& caller)
    {
        if (call.kind == IROperationKind::ForeignCall) {
            auto result = behavior(IROwnershipBehavior::Unknown, call.origin,
                "reference crosses a foreign-call boundary");
            merge_into(result, behavior(IROwnershipBehavior::Escape, call.origin,
                "foreign call may retain or store the reference"));
            return result;
        }
        if (call.selected_declaration.empty()) {
            if (call.name == "print" || runtime_intrinsic(call.name)) return {};
            auto result = behavior(IROwnershipBehavior::Unknown, call.origin,
                "callee ownership behavior is unavailable");
            merge_into(result, behavior(IROwnershipBehavior::Escape, call.origin,
                "unknown call may retain or store the reference"));
            return result;
        }
        const auto targets = candidates(call);
        if (targets.empty()) {
            auto result = behavior(IROwnershipBehavior::Unknown, call.origin,
                "dynamic ownership targets are unavailable");
            merge_into(result, behavior(IROwnershipBehavior::Escape, call.origin,
                "unknown dynamic target may retain the reference"));
            return result;
        }
        UseResult result;
        for (const auto* target : targets) {
            auto* target_node = node_for(call, caller, *target);
            if (!target_node) {
                merge_into(result, behavior(IROwnershipBehavior::Unknown,
                    call.origin, "callee ownership summary is unavailable",
                    target->identity, true));
                continue;
            }
            const bool receiver = !target->owner_identity.empty()
                && (target->kind == IRDeclarationKind::Method
                    || target->kind == IRDeclarationKind::Conversion);
            const auto* parameter = parameter_for(
                *target_node, child_index, receiver);
            if (!parameter) continue;
            for (const auto value : behavior_order) {
                const auto index = behavior_index(value);
                if (value == IROwnershipBehavior::Observe
                    || !parameter->behaviors.test(index)) continue;
                merge_into(result, behavior(value, call.origin,
                    "propagated callee parameter ownership behavior",
                    target_node->identity, true));
            }
        }
        return result;
    }

    bool weak_field(const IROperation& operation) const
    {
        if (operation.children.empty()
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

    UseResult classify_parent(
        const IROperation& parent,
        std::size_t child_index,
        const Node& node)
    {
        switch (parent.kind) {
        case IROperationKind::MemberAccess:
        case IROperationKind::IndexAccess:
            if (child_index == 0) return {};
            break;
        case IROperationKind::MemberAssign:
            if (child_index == 0) return {};
            if (weak_field(parent)) {
                return behavior(IROwnershipBehavior::WeakStore, parent.origin,
                    "stored in a non-owning zeroing weak field");
            } else {
                auto result = behavior(IROwnershipBehavior::Store, parent.origin,
                    "stored in a strong field");
                merge_into(result, behavior(IROwnershipBehavior::Escape,
                    parent.origin, "strong field outlives the call"));
                return result;
            }
        case IROperationKind::IndexAssign:
            if (child_index + 1 == parent.children.size()) {
                auto result = behavior(IROwnershipBehavior::Store, parent.origin,
                    "stored in a collection");
                merge_into(result, behavior(IROwnershipBehavior::Escape,
                    parent.origin, "collection storage outlives the call"));
                return result;
            }
            return {};
        case IROperationKind::CollectionOperation:
            if ((parent.name == "Array" || parent.name == "List"
                    || parent.name == "Map" || parent.name == "add"
                    || parent.name == "append" || parent.name == "insert")
                && !(child_index == 0
                    && parent.name != "Array" && parent.name != "List"
                    && parent.name != "Map")) {
                auto result = behavior(IROwnershipBehavior::Store, parent.origin,
                    "stored in a collection");
                merge_into(result, behavior(IROwnershipBehavior::Escape,
                    parent.origin, "collection storage outlives the call"));
                return result;
            }
            return {};
        case IROperationKind::Call:
        case IROperationKind::VirtualCall:
        case IROperationKind::InterfaceCall:
        case IROperationKind::ForeignCall:
        case IROperationKind::Conversion:
            return classify_call(parent, child_index, node);
        case IROperationKind::Return: {
            auto result = behavior(IROwnershipBehavior::Return, parent.origin,
                "returned from the callable");
            merge_into(result, behavior(IROwnershipBehavior::Escape,
                parent.origin, "return value escapes the callable"));
            return result;
        }
        case IROperationKind::Declare:
        case IROperationKind::Assign:
            return behavior(IROwnershipBehavior::Retain, parent.origin,
                "copied into an owning local");
        case IROperationKind::EnumConstruction:
        case IROperationKind::ResultConstruction:
            return behavior(IROwnershipBehavior::Retain, parent.origin,
                "retained by a managed payload");
        case IROperationKind::Binary:
            if (parent.name == "==" || parent.name == "!=") return {};
            break;
        default:
            break;
        }
        auto result = behavior(IROwnershipBehavior::Unknown, parent.origin,
            "reference use is not proven operation-bounded");
        merge_into(result, behavior(IROwnershipBehavior::Escape, parent.origin,
            "unknown use may retain the reference"));
        return result;
    }

    UseResult classify_name(
        const IROperation& operation,
        std::string_view name,
        const Node& node,
        const IROperation* parent = nullptr,
        std::size_t child_index = 0)
    {
        if (operation.kind == IROperationKind::Load && operation.name == name) {
            return parent ? classify_parent(*parent, child_index, node)
                          : behavior(IROwnershipBehavior::Unknown,
                                operation.origin,
                                "reference load has no bounded consumer");
        }
        UseResult result;
        for (std::size_t index = 0; index < operation.children.size(); ++index) {
            merge_into(result, classify_name(operation.children[index], name,
                node, &operation, index));
        }
        if (operation.kind == IROperationKind::Return
            && !operation.children.empty()
            && (operation.children.front().kind
                    == IROperationKind::EnumConstruction
                || operation.children.front().kind
                    == IROperationKind::ResultConstruction)
            && result.behaviors.test(
                behavior_index(IROwnershipBehavior::Retain))) {
            merge_into(result, behavior(IROwnershipBehavior::Return,
                operation.origin, "returned inside a managed payload"));
            merge_into(result, behavior(IROwnershipBehavior::Escape,
                operation.origin,
                "returned managed payload escapes the callable"));
        }
        return result;
    }

    static bool root_is_name(
        const IROperation& operation,
        std::string_view name)
    {
        if (operation.kind == IROperationKind::Load) return operation.name == name;
        if (operation.kind == IROperationKind::MemberAccess
            && !operation.children.empty()) {
            return root_is_name(operation.children.front(), name);
        }
        return false;
    }

    IRReturnOwnershipSummary classify_return_expression(
        const IROperation& expression,
        const Node& node,
        const IROrigin& origin)
    {
        if (expression.kind == IROperationKind::Construction) {
            return {IRReturnOwnership::NewOwned, std::nullopt, origin,
                "returns a newly constructed owning reference"};
        }
        for (const auto& parameter : node.parameters) {
            if (expression.kind == IROperationKind::Load
                && expression.name == parameter.name) {
                return {
                    parameter.receiver ? IRReturnOwnership::ReceiverAlias
                                       : IRReturnOwnership::ParameterAlias,
                    parameter.index, origin,
                    "returns the incoming reference",
                };
            }
            if (expression.kind == IROperationKind::MemberAccess
                && root_is_name(expression, parameter.name)) {
                return {IRReturnOwnership::FieldAlias, parameter.index, origin,
                    "returns a reference read from an input field"};
            }
        }
        if ((expression.kind == IROperationKind::Call
                || expression.kind == IROperationKind::VirtualCall
                || expression.kind == IROperationKind::InterfaceCall)
            && !expression.selected_declaration.empty()) {
            const auto targets = candidates(expression);
            IRReturnOwnershipSummary result;
            for (const auto* target : targets) {
                const auto* target_node = node_for(expression, node, *target);
                if (!target_node) {
                    return {IRReturnOwnership::Unknown, std::nullopt, origin,
                        "callee return ownership is unavailable"};
                }
                auto value = target_node->return_value;
                if (value.ownership == IRReturnOwnership::None) {
                    return {};
                }
                if (value.ownership == IRReturnOwnership::ParameterAlias
                    && value.parameter_index
                    && *value.parameter_index < expression.children.size()) {
                    const bool receiver = !target->owner_identity.empty();
                    const std::size_t child = receiver
                        ? *value.parameter_index + 1 : *value.parameter_index;
                    if (child < expression.children.size()) {
                        value = classify_return_expression(
                            expression.children[child], node, origin);
                    }
                } else if (value.ownership
                        == IRReturnOwnership::ReceiverAlias
                    && !expression.children.empty()) {
                    value = classify_return_expression(
                        expression.children.front(), node, origin);
                }
                if (result.ownership == IRReturnOwnership::None) result = value;
                else if (result.ownership != value.ownership
                    || result.parameter_index != value.parameter_index) {
                    return {IRReturnOwnership::Unknown, std::nullopt, origin,
                        "reachable callees return different ownership relationships"};
                }
            }
            if (result.ownership != IRReturnOwnership::None) return result;
        }
        return {IRReturnOwnership::Unknown, std::nullopt, origin,
            "return ownership relationship is not proven"};
    }

    void find_returns(
        const IROperation& operation,
        const Node& node,
        IRReturnOwnershipSummary& result)
    {
        if (operation.kind == IROperationKind::Return
            && !operation.children.empty()) {
            const auto value = classify_return_expression(
                operation.children.front(), node, operation.origin);
            if (value.ownership == IRReturnOwnership::None) {
                return;
            }
            if (result.ownership == IRReturnOwnership::None) result = value;
            else if (result.ownership != value.ownership
                || result.parameter_index != value.parameter_index) {
                result = {IRReturnOwnership::Unknown, std::nullopt,
                    operation.origin,
                    "reachable returns have different ownership relationships"};
            }
        }
        for (const auto& child : operation.children) find_returns(child, node, result);
    }

    bool update_node(Node& node)
    {
        if (!node.declaration->body) return false;
        bool changed = false;
        for (auto& parameter : node.parameters) {
            const auto result = classify_name(
                *node.declaration->body, parameter.name, node);
            const auto combined = parameter.behaviors | result.behaviors;
            if (combined != parameter.behaviors) {
                parameter.behaviors = combined;
                changed = true;
            }
            for (std::size_t index = 0; index < kBehaviorCount; ++index) {
                if (!parameter.causes[index] && result.causes[index]) {
                    parameter.causes[index] = result.causes[index];
                }
            }
        }
        const bool tracks_return = node.declaration->return_type
            && (is_reference(concrete(*node.declaration->return_type, node))
                || (node.substitutions.empty()
                    && generic_parameter(*node.declaration,
                        *node.declaration->return_type)));
        IRReturnOwnershipSummary returns;
        if (tracks_return) {
            find_returns(*node.declaration->body, node, returns);
        }
        if (returns.ownership != IRReturnOwnership::None
            && (node.return_value.ownership == IRReturnOwnership::None
                || node.return_value.ownership != returns.ownership
                || node.return_value.parameter_index != returns.parameter_index)) {
            const auto next = node.return_value.ownership == IRReturnOwnership::None
                ? returns
                : IRReturnOwnershipSummary{IRReturnOwnership::Unknown,
                    std::nullopt, returns.origin,
                    "return ownership became conservative during propagation"};
            if (node.return_value.ownership != next.ownership
                || node.return_value.parameter_index != next.parameter_index) {
                node.return_value = next;
                changed = true;
            }
        }
        return changed;
    }

    void stabilize()
    {
        bool changed = true;
        while (changed) {
            changed = false;
            for (auto& node : nodes_) changed = update_node(node) || changed;
        }
        for (auto& node : nodes_) {
            if (node.declaration->return_type
                && is_reference(concrete(*node.declaration->return_type, node))
                && node.return_value.ownership == IRReturnOwnership::None) {
                node.return_value = {IRReturnOwnership::Unknown, std::nullopt,
                    node.declaration->origin,
                    "managed return ownership is unavailable"};
            }
        }
    }

    void publish()
    {
        program_.ownership_summaries.reserve(nodes_.size());
        for (const auto& node : nodes_) {
            IRCallableOwnershipSummary summary;
            summary.callable_identity = node.identity;
            summary.return_value = node.return_value;
            for (const auto& parameter : node.parameters) {
                IRParameterOwnershipSummary value;
                value.parameter_name = parameter.name;
                value.parameter_index = parameter.index;
                value.origin = parameter.origin;
                value.receiver = parameter.receiver;
                for (const auto behavior_value : behavior_order) {
                    const auto index = behavior_index(behavior_value);
                    if (behavior_value == IROwnershipBehavior::Observe) continue;
                    if (!parameter.behaviors.test(index)) continue;
                    value.behaviors.push_back(behavior_value);
                    if (parameter.causes[index]) {
                        value.causes.push_back(*parameter.causes[index]);
                    }
                }
                normalize(value);
                summary.parameters.push_back(std::move(value));
            }
            program_.ownership_summaries.push_back(std::move(summary));
        }
    }

    IRProgram& program_;
    std::vector<Node> nodes_;
    std::unordered_map<std::string, std::size_t> indices_;
    std::unordered_map<std::string, const IRDeclaration*> declarations_;
    std::unordered_map<std::string, const IRDeclaration*> nominals_;
};

} // namespace

void OwnershipSummaryAnalyzer::analyze(IRProgram& program) const
{
    Analysis(program).run();
}

const IRCallableOwnershipSummary* find_ownership_summary(
    const IRProgram& program,
    const IROperation& call)
{
    if (call.selected_declaration.empty()) return nullptr;
    if (!call.substitutions.empty()) {
        const auto identity = specialization_identity(call);
        const auto found = std::ranges::find_if(program.ownership_summaries,
            [&](const auto& summary) {
                return summary.callable_identity == identity;
            });
        if (found != program.ownership_summaries.end()) return &*found;
    }
    const auto found = std::ranges::find_if(program.ownership_summaries,
        [&](const auto& summary) {
            return summary.callable_identity == call.selected_declaration;
        });
    return found == program.ownership_summaries.end() ? nullptr : &*found;
}

std::optional<IRReturnOwnershipSummary> ownership_for_call_result(
    const IRProgram& program,
    const IROperation& call)
{
    Resolver resolver(program);
    const auto targets = resolver.candidates(call);
    if (targets.empty()) return std::nullopt;
    std::optional<IRReturnOwnershipSummary> result;
    for (const auto* target : targets) {
        const auto* summary = resolver.summary_for(call, *target);
        if (!summary) return std::nullopt;
        if (!result) {
            result = summary->return_value;
            continue;
        }
        if (result->ownership != summary->return_value.ownership
            || result->parameter_index
                != summary->return_value.parameter_index) {
            return IRReturnOwnershipSummary{
                IRReturnOwnership::Unknown,
                std::nullopt,
                call.origin,
                "reachable callees return different ownership relationships",
            };
        }
    }
    return result;
}

std::optional<IRParameterOwnershipSummary> ownership_for_call_operand(
    const IRProgram& program,
    const IROperation& call,
    std::size_t operand_index)
{
    Resolver resolver(program);
    const auto targets = resolver.candidates(call);
    if (targets.empty()) return std::nullopt;
    IRParameterOwnershipSummary result;
    result.parameter_index = operand_index;
    result.origin = call.origin;
    bool found_any = false;
    for (const auto* target : targets) {
        const auto* summary = resolver.summary_for(call, *target);
        if (!summary) return std::nullopt;
        const bool receiver = !target->owner_identity.empty()
            && (target->kind == IRDeclarationKind::Method
                || target->kind == IRDeclarationKind::Conversion);
        const std::size_t parameter_index = receiver
            ? (operand_index == 0 ? kReceiverIndex : operand_index - 1)
            : operand_index;
        const auto parameter = std::ranges::find_if(summary->parameters,
            [&](const auto& candidate) {
                return candidate.parameter_index == parameter_index;
            });
        if (parameter == summary->parameters.end()) continue;
        if (!found_any) {
            result.parameter_name = parameter->parameter_name;
            result.receiver = parameter->receiver;
            found_any = true;
        }
        for (const auto value : parameter->behaviors) {
            add_behavior(result, value);
        }
        for (const auto& cause : parameter->causes) {
            auto transitive = cause;
            transitive.transitive = true;
            if (transitive.via_callable.empty()) {
                transitive.via_callable = summary->callable_identity;
            }
            add_behavior(result, cause.behavior, &transitive);
        }
    }
    if (!found_any) return std::nullopt;
    normalize(result);
    return result;
}

bool ownership_is_observed_only(
    const IRParameterOwnershipSummary& summary)
{
    return summary.behaviors.size() == 1
        && summary.behaviors.front() == IROwnershipBehavior::Observe;
}

bool ownership_may_escape(const IRParameterOwnershipSummary& summary)
{
    return has_behavior(summary, IROwnershipBehavior::Store)
        || has_behavior(summary, IROwnershipBehavior::Return)
        || has_behavior(summary, IROwnershipBehavior::Consume)
        || has_behavior(summary, IROwnershipBehavior::Escape)
        || has_behavior(summary, IROwnershipBehavior::Unknown);
}

} // namespace toro
