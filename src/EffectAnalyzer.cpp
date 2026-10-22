#include "toro/EffectAnalyzer.hpp"

#include "toro/StandardLibrary.hpp"

#include <array>
#include <bitset>
#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace toro {
namespace {

constexpr std::size_t effect_index(IREffect effect)
{
    return static_cast<std::size_t>(effect);
}

constexpr std::size_t effect_count = effect_index(IREffect::Count);
using EffectBits = std::bitset<effect_count>;

constexpr std::array effects_in_order{
    IREffect::Allocate,
    IREffect::Retain,
    IREffect::ReleaseDestroy,
    IREffect::Escape,
    IREffect::Block,
    IREffect::FilesystemIO,
    IREffect::EnvironmentAccess,
    IREffect::TimeQuery,
    IREffect::ProcessIO,
    IREffect::Synchronize,
    IREffect::ForeignCall,
    IREffect::NativeCall,
    IREffect::DynamicDispatch,
    IREffect::Terminate,
    IREffect::UnknownExternal,
};

std::string type_key(const Type& type)
{
    std::string result = type.kind == TypeKind::Unknown && !type.name.empty()
        ? type.name
        : std::string(base_type_name(type));
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
    const std::vector<std::pair<std::string, IRType>>& caller_substitutions = {})
{
    std::string result = operation.selected_declaration;
    if (operation.substitutions.empty()) return result;
    result += '<';
    for (std::size_t index = 0; index < operation.substitutions.size(); ++index) {
        if (index != 0) result += ',';
        const auto& concrete = substitute_type(
            operation.substitutions[index].second, caller_substitutions);
        result += operation.substitutions[index].first + '='
            + type_key(concrete.type);
    }
    result += '>';
    return result;
}

bool is_callable(const IRDeclaration& declaration)
{
    return declaration.kind == IRDeclarationKind::Function
        || declaration.kind == IRDeclarationKind::ForeignFunction
        || declaration.kind == IRDeclarationKind::Method
        || declaration.kind == IRDeclarationKind::Conversion;
}

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

std::string method_suffix(std::string_view identity)
{
    const auto separator = identity.rfind("::");
    return separator == std::string_view::npos
        ? std::string(identity) : std::string(identity.substr(separator + 2));
}

struct EffectEdge {
    std::string target;
    IROrigin origin;
};

struct EffectNode {
    std::string identity;
    const IRDeclaration* declaration{nullptr};
    EffectBits local_effects;
    EffectBits effects;
    std::array<std::optional<IROrigin>, effect_count> local_origins;
    std::vector<IRLocalEffectSite> local_effect_sites;
    std::vector<EffectEdge> edges;
    std::vector<std::pair<std::string, IRType>> substitutions;
};

class Analysis {
public:
    explicit Analysis(IRProgram& program)
        : program_(program)
    {
    }

    void run()
    {
        program_.effect_summaries.clear();
        collect_declarations();
        collect_specializations();
        for (auto& node : nodes_) analyze_node(node);
        propagate();
        publish();
    }

private:
    void collect_declarations()
    {
        for (const auto& declaration : program_.declarations) {
            declarations_.emplace(declaration.identity, &declaration);
            if (declaration.kind == IRDeclarationKind::Class
                || declaration.kind == IRDeclarationKind::Struct
                || declaration.kind == IRDeclarationKind::Interface) {
                nominals_.emplace(declaration.identity, &declaration);
            }
            if (is_callable(declaration)) {
                add_node(declaration.identity, declaration, {});
            }
        }
    }

    void collect_specializations()
    {
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

    void collect_specializations(const IROperation& operation)
    {
        if (!operation.selected_declaration.empty()
            && !operation.substitutions.empty()) {
            const auto declaration = declarations_.find(
                operation.selected_declaration);
            if (declaration != declarations_.end()) {
                add_node(
                    specialization_identity(operation),
                    *declaration->second,
                    operation.substitutions);
            }
        }
        for (const auto& child : operation.children) {
            collect_specializations(child);
        }
    }

    void add_node(
        const std::string& identity,
        const IRDeclaration& declaration,
        const std::vector<std::pair<std::string, IRType>>& substitutions)
    {
        if (node_indices_.contains(identity)) return;
        node_indices_.emplace(identity, nodes_.size());
        EffectNode node;
        node.identity = identity;
        node.declaration = &declaration;
        node.substitutions = substitutions;
        nodes_.push_back(std::move(node));
    }

    void add_local(
        EffectNode& node,
        IREffect effect,
        const IROrigin& origin,
        std::optional<IROperationKind> operation_kind = std::nullopt,
        std::string operation_name = {})
    {
        const auto index = effect_index(effect);
        node.local_effects.set(index);
        if (!node.local_origins[index]) node.local_origins[index] = origin;
        node.local_effect_sites.push_back(IRLocalEffectSite{
            effect,
            origin,
            operation_kind,
            std::move(operation_name),
        });
    }

    void add_operation_effect(
        EffectNode& node,
        IREffect effect,
        const IROperation& operation)
    {
        add_local(node, effect, operation.origin,
            operation.kind, operation.name);
    }

    void add_edge(
        EffectNode& node,
        const std::string& target,
        const IROrigin& origin)
    {
        if (node_indices_.contains(target)) {
            node.edges.push_back(EffectEdge{target, origin});
        } else {
            add_local(node, IREffect::UnknownExternal, origin);
        }
    }

    void analyze_node(EffectNode& node)
    {
        const auto& declaration = *node.declaration;
        for (const auto& parameter : declaration.parameters) {
            if (is_shared_reference(concrete_type(parameter.type, node))) {
                add_local(node, IREffect::Retain, parameter.origin);
                add_local(node, IREffect::ReleaseDestroy, parameter.origin);
            }
        }
        if (declaration.kind == IRDeclarationKind::ForeignFunction) {
            add_local(node, IREffect::ForeignCall, declaration.origin);
            add_local(node, IREffect::UnknownExternal, declaration.origin);
        } else if (!declaration.body) {
            if (declaration.kind == IRDeclarationKind::Method
                && declaration.is_abstract) {
                add_local(node, IREffect::UnknownExternal, declaration.origin);
            }
        } else {
            analyze_operation(*declaration.body, node);
        }
        node.effects = node.local_effects;
    }

    const IRType& concrete_type(
        const IRType& type,
        const EffectNode& node) const
    {
        return substitute_type(type, node.substitutions);
    }

    void add_intrinsic_effects(
        RuntimeIntrinsic intrinsic,
        const IROperation& operation,
        EffectNode& node)
    {
        add_operation_effect(node, IREffect::NativeCall, operation);
        switch (intrinsic) {
        case RuntimeIntrinsic::IoWriteLine:
            add_operation_effect(node, IREffect::Block, operation);
            add_operation_effect(node, IREffect::ProcessIO, operation);
            return;
        case RuntimeIntrinsic::FsExists:
            add_operation_effect(node, IREffect::Block, operation);
            add_operation_effect(node, IREffect::FilesystemIO, operation);
            return;
        case RuntimeIntrinsic::FsWriteText:
        case RuntimeIntrinsic::FsRemoveFile:
            add_operation_effect(node, IREffect::Allocate, operation);
            add_operation_effect(node, IREffect::Block, operation);
            add_operation_effect(node, IREffect::FilesystemIO, operation);
            return;
        case RuntimeIntrinsic::FsReadText:
            add_operation_effect(node, IREffect::Allocate, operation);
            add_operation_effect(node, IREffect::Block, operation);
            add_operation_effect(node, IREffect::FilesystemIO, operation);
            return;
        case RuntimeIntrinsic::EnvHas:
            add_operation_effect(node, IREffect::EnvironmentAccess, operation);
            return;
        case RuntimeIntrinsic::EnvGet:
            add_operation_effect(node, IREffect::Allocate, operation);
            add_operation_effect(node, IREffect::EnvironmentAccess, operation);
            return;
        case RuntimeIntrinsic::ProcessExit:
            add_operation_effect(node, IREffect::ProcessIO, operation);
            add_operation_effect(node, IREffect::Terminate, operation);
            return;
        case RuntimeIntrinsic::TimeWallMillis:
            add_operation_effect(node, IREffect::TimeQuery, operation);
            return;
        case RuntimeIntrinsic::StringConcat:
        case RuntimeIntrinsic::StringSubstring:
            add_operation_effect(node, IREffect::Allocate, operation);
            return;
        case RuntimeIntrinsic::StringLength:
        case RuntimeIntrinsic::StringContains:
        case RuntimeIntrinsic::StringStartsWith:
        case RuntimeIntrinsic::StringEndsWith:
            return;
        }
    }

    void analyze_operation(const IROperation& operation, EffectNode& node)
    {
        switch (operation.kind) {
        case IROperationKind::Construction:
            if (operation.result_type
                && concrete_type(*operation.result_type, node).category
                    == IRValueCategory::ManagedReference) {
                add_operation_effect(node, IREffect::Allocate, operation);
                add_operation_effect(node, IREffect::Retain, operation);
                add_operation_effect(node, IREffect::ReleaseDestroy, operation);
            }
            break;
        case IROperationKind::CollectionOperation:
            if (operation.name == "Array" || operation.name == "List"
                || operation.name == "Map" || operation.name == "add"
                || operation.name == "append" || operation.name == "clone"
                || operation.name == "insert") {
                add_operation_effect(node, IREffect::Allocate, operation);
            }
            if (operation.result_type
                && owns_runtime_storage(
                    concrete_type(*operation.result_type, node))) {
                add_operation_effect(node, IREffect::ReleaseDestroy, operation);
                if (is_shared_reference(
                        concrete_type(*operation.result_type, node))) {
                    add_operation_effect(node, IREffect::Retain, operation);
                }
            }
            if (operation.name == "Array" || operation.name == "List"
                || operation.name == "Map" || operation.name == "add"
                || operation.name == "append" || operation.name == "insert") {
                for (const auto& child : operation.children) {
                    if (child.result_type
                        && is_shared_reference(
                            concrete_type(*child.result_type, node))) {
                        add_operation_effect(node, IREffect::Retain, operation);
                        add_operation_effect(
                            node, IREffect::ReleaseDestroy, operation);
                        add_operation_effect(node, IREffect::Escape, operation);
                        break;
                    }
                }
            }
            break;
        case IROperationKind::Declare:
        case IROperationKind::Assign: {
            const IRType* stored_type = operation.result_type
                ? &*operation.result_type : nullptr;
            if (!stored_type && !operation.children.empty()
                && operation.children.back().result_type) {
                stored_type = &*operation.children.back().result_type;
            }
            if (stored_type
                && owns_runtime_storage(concrete_type(*stored_type, node))) {
                add_operation_effect(node, IREffect::ReleaseDestroy, operation);
                if (is_shared_reference(concrete_type(*stored_type, node))) {
                    add_operation_effect(node, IREffect::Retain, operation);
                }
            }
            break;
        }
        case IROperationKind::MemberAssign:
        case IROperationKind::IndexAssign:
            if (!operation.children.empty()) {
                const auto& value = operation.children.back();
                if (value.result_type
                    && is_shared_reference(
                        concrete_type(*value.result_type, node))) {
                    add_operation_effect(node, IREffect::Retain, operation);
                    add_operation_effect(
                        node, IREffect::ReleaseDestroy, operation);
                    add_operation_effect(node, IREffect::Escape, operation);
                }
            }
            if (operation.kind == IROperationKind::IndexAssign
                && !operation.children.empty()
                && operation.children.front().result_type
                && operation.children.front().result_type->type.name == "Map") {
                add_operation_effect(node, IREffect::Allocate, operation);
            }
            break;
        case IROperationKind::Return:
            if (!operation.children.empty()
                && operation.children.front().result_type
                && is_shared_reference(concrete_type(
                    *operation.children.front().result_type, node))) {
                add_operation_effect(node, IREffect::Escape, operation);
            }
            break;
        case IROperationKind::ForeignCall:
        case IROperationKind::Call:
        case IROperationKind::Conversion:
            analyze_call(operation, node);
            break;
        case IROperationKind::VirtualCall:
        case IROperationKind::InterfaceCall:
            add_operation_effect(node, IREffect::DynamicDispatch, operation);
            analyze_dynamic_call(operation, node);
            break;
        case IROperationKind::EnumConstruction:
        case IROperationKind::ResultConstruction:
            for (const auto& child : operation.children) {
                if (child.result_type
                    && is_shared_reference(
                        concrete_type(*child.result_type, node))) {
                    add_operation_effect(node, IREffect::Retain, operation);
                    add_operation_effect(
                        node, IREffect::ReleaseDestroy, operation);
                    break;
                }
            }
            break;
        case IROperationKind::Block:
        case IROperationKind::Constant:
        case IROperationKind::Load:
        case IROperationKind::Unary:
        case IROperationKind::Binary:
        case IROperationKind::MemberAccess:
        case IROperationKind::IndexAccess:
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
            analyze_operation(child, node);
        }
    }

    void analyze_call(
        const IROperation& operation,
        EffectNode& node)
    {
        if (!operation.selected_declaration.empty()) {
            const auto concrete = specialization_identity(
                operation, node.substitutions);
            if (node_indices_.contains(concrete)) {
                add_edge(node, concrete, operation.origin);
            } else {
                const auto symbolic = specialization_identity(operation);
                if (node_indices_.contains(symbolic)) {
                    add_edge(node, symbolic, operation.origin);
                } else {
                    add_edge(node, operation.selected_declaration, operation.origin);
                }
            }
        } else if (const auto intrinsic = runtime_intrinsic(operation.name)) {
            add_intrinsic_effects(*intrinsic, operation, node);
        } else if (operation.name == "print") {
            add_operation_effect(node, IREffect::NativeCall, operation);
            add_operation_effect(node, IREffect::Block, operation);
            add_operation_effect(node, IREffect::ProcessIO, operation);
        } else {
            add_operation_effect(node, IREffect::UnknownExternal, operation);
        }

        for (const auto& child : operation.children) {
            if (child.result_type
                && is_shared_reference(concrete_type(*child.result_type, node))) {
                add_operation_effect(node, IREffect::Retain, operation);
                add_operation_effect(node, IREffect::ReleaseDestroy, operation);
                break;
            }
        }
    }

    void analyze_dynamic_call(const IROperation& operation, EffectNode& node)
    {
        const std::string suffix = method_suffix(operation.selected_declaration);
        const auto selected = declarations_.find(operation.selected_declaration);
        const std::string selected_owner = selected != declarations_.end()
            ? selected->second->owner_identity : std::string{};
        bool found = false;
        for (const auto& candidate : program_.declarations) {
            if (candidate.kind != IRDeclarationKind::Method || !candidate.body
                || candidate.name != operation.name
                || method_suffix(candidate.identity) != suffix) {
                continue;
            }
            bool valid_candidate = false;
            if (operation.kind == IROperationKind::VirtualCall) {
                valid_candidate = is_descendant(
                    candidate.owner_identity, selected_owner);
            } else {
                for (const auto& [identity, nominal] : nominals_) {
                    if ((nominal->kind == IRDeclarationKind::Class
                            || nominal->kind == IRDeclarationKind::Struct)
                        && implements_interface(identity, selected_owner)
                        && is_descendant(identity, candidate.owner_identity)) {
                        valid_candidate = true;
                        break;
                    }
                }
            }
            if (!valid_candidate) continue;
            add_edge(node, candidate.identity, operation.origin);
            found = true;
        }
        if (!found) {
            add_operation_effect(node, IREffect::UnknownExternal, operation);
        }
        for (const auto& child : operation.children) {
            if (child.result_type
                && is_shared_reference(concrete_type(*child.result_type, node))) {
                add_operation_effect(node, IREffect::Retain, operation);
                add_operation_effect(node, IREffect::ReleaseDestroy, operation);
                break;
            }
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
        if (identity.empty() || interface_identity.empty()) return false;
        const auto nominal = nominals_.find(std::string(identity));
        if (nominal == nominals_.end()) return false;
        for (const auto& related : nominal->second->related_types) {
            if (related.type.name == interface_identity) return true;
        }
        const auto base = class_base(identity);
        return !base.empty() && implements_interface(base, interface_identity);
    }

    void propagate()
    {
        bool changed = true;
        while (changed) {
            changed = false;
            for (auto& node : nodes_) {
                auto combined = node.local_effects;
                for (const auto& edge : node.edges) {
                    combined |= nodes_[node_indices_.at(edge.target)].effects;
                }
                if (combined != node.effects) {
                    node.effects = combined;
                    changed = true;
                }
            }
        }
    }

    void publish()
    {
        program_.effect_summaries.reserve(nodes_.size());
        for (const auto& node : nodes_) {
            IRCallableEffectSummary summary;
            summary.callable_identity = node.identity;
            summary.local_effect_sites = node.local_effect_sites;
            for (const auto& edge : node.edges) {
                summary.call_edges.push_back(
                    IREffectCallEdge{edge.target, edge.origin});
            }
            for (const auto effect : effects_in_order) {
                const auto index = effect_index(effect);
                if (!node.effects.test(index)) continue;
                summary.effects.push_back(effect);
                if (node.local_origins[index]) {
                    summary.causes.push_back(IREffectCause{
                        effect, *node.local_origins[index], {}, false});
                    continue;
                }
                const EffectEdge* selected_edge = nullptr;
                for (const auto& edge : node.edges) {
                    const auto& target = nodes_[node_indices_.at(edge.target)];
                    if (!target.effects.test(index)) continue;
                    if (!selected_edge) selected_edge = &edge;
                    if (target.local_effects.test(index)) {
                        selected_edge = &edge;
                        break;
                    }
                    if (selected_edge->target == node.identity
                        && edge.target != node.identity) {
                        selected_edge = &edge;
                    }
                }
                if (selected_edge) {
                    summary.causes.push_back(IREffectCause{
                        effect,
                        selected_edge->origin,
                        selected_edge->target,
                        true,
                    });
                }
            }
            program_.effect_summaries.push_back(std::move(summary));
        }
    }

    IRProgram& program_;
    std::vector<EffectNode> nodes_;
    std::unordered_map<std::string, std::size_t> node_indices_;
    std::unordered_map<std::string, const IRDeclaration*> declarations_;
    std::unordered_map<std::string, const IRDeclaration*> nominals_;
};

} // namespace

void EffectAnalyzer::analyze(IRProgram& program) const
{
    Analysis(program).run();
}

} // namespace toro
