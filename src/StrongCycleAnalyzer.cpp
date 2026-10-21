#include "toro/StrongCycleAnalyzer.hpp"

#include "toro/ModuleIdentity.hpp"

#include <algorithm>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace toro {
namespace {

std::string type_key(const Type& type, bool include_outer_nullability = true)
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
    if (include_outer_nullability && type.nullable) result += '?';
    return result;
}

Type nonnullable(Type type)
{
    type.nullable = false;
    return type;
}

bool is_concrete(const Type& type)
{
    if (type.deferred) return false;
    return std::ranges::all_of(type.arguments,
        [](const Type& argument) { return is_concrete(argument); });
}

Type substitute(
    const Type& type,
    const std::map<std::string, Type>& substitutions)
{
    if (type.kind == TypeKind::Unknown && type.arguments.empty()) {
        if (const auto found = substitutions.find(type.name);
            found != substitutions.end()) {
            Type result = found->second;
            if (type.nullable) result.nullable = true;
            return result;
        }
    }
    Type result = type;
    result.arguments.clear();
    result.deferred = false;
    for (const auto& argument : type.arguments) {
        result.arguments.push_back(substitute(argument, substitutions));
        result.deferred = result.deferred || result.arguments.back().deferred;
    }
    return result;
}

std::map<std::string, Type> substitutions_for(
    const IRDeclaration& declaration,
    const Type& instance)
{
    std::map<std::string, Type> result;
    const auto count = std::min(
        declaration.generic_parameters.size(), instance.arguments.size());
    for (std::size_t index = 0; index < count; ++index) {
        result.emplace(
            declaration.generic_parameters[index].name,
            instance.arguments[index]);
    }
    return result;
}

DiagnosticSource diagnostic_source(const IROrigin& origin)
{
    return DiagnosticSource{origin.module_name, origin.location};
}

class Analysis {
public:
    explicit Analysis(IRProgram& program)
        : program_(program)
    {
        for (const auto& declaration : program.declarations) {
            declarations_.emplace(declaration.identity, &declaration);
            if (declaration.kind == IRDeclarationKind::Class) {
                classes_.emplace(declaration.identity, &declaration);
                ++source_name_counts_[declaration.name];
            } else if (declaration.kind == IRDeclarationKind::Struct) {
                structs_.emplace(declaration.identity, &declaration);
                ++source_name_counts_[declaration.name];
            } else if (declaration.kind == IRDeclarationKind::Enum) {
                enums_.emplace(declaration.identity, &declaration);
                ++source_name_counts_[declaration.name];
            } else if (declaration.kind == IRDeclarationKind::Interface) {
                ++source_name_counts_[declaration.name];
            }
        }
    }

    void run()
    {
        program_.strong_ownership_edges.clear();
        program_.strong_reference_cycles.clear();
        std::erase_if(program_.diagnostics, [](const Diagnostic& diagnostic) {
            return diagnostic.code == "strong-reference-cycle";
        });

        collect_initial_instances();
        close_instances();
        build_edges();
        detect_cycles();
    }

private:
    struct FieldView {
        const IRField* field{nullptr};
        Type type{TypeKind::Unknown};
    };

    std::string display(const Type& type) const
    {
        std::string result;
        if (type.kind == TypeKind::Unknown && !type.name.empty()) {
            if (const auto found = declarations_.find(type.name);
                found != declarations_.end()) {
                result = found->second->name;
                if (source_name_counts_.at(found->second->name) > 1
                    && !found->second->origin.module_name.empty()) {
                    result = found->second->origin.module_name + '.' + result;
                }
            } else {
                result = std::string(declaration_source_name(type.name));
            }
        } else {
            result = std::string(base_type_name(type));
        }
        if (!type.arguments.empty()) {
            result += '<';
            for (std::size_t index = 0; index < type.arguments.size(); ++index) {
                if (index != 0) result += ", ";
                result += display(type.arguments[index]);
            }
            result += '>';
        }
        if (type.nullable) result += '?';
        return result;
    }

    bool add_instance(const Type& candidate)
    {
        const auto declaration = classes_.find(candidate.name);
        if (declaration == classes_.end()) return false;
        if (!is_concrete(candidate)
            || candidate.arguments.size()
                != declaration->second->generic_parameters.size()) {
            return false;
        }
        Type instance = nonnullable(candidate);
        return instances_.emplace(type_key(instance, false), std::move(instance)).second;
    }

    void scan_type(const Type& type)
    {
        if (classes_.contains(type.name)) add_instance(type);
        for (const auto& argument : type.arguments) scan_type(argument);
    }

    void scan_operation(const IROperation& operation)
    {
        if (operation.result_type) scan_type(operation.result_type->type);
        for (const auto& [name, type] : operation.substitutions) {
            static_cast<void>(name);
            scan_type(type.type);
        }
        for (const auto& child : operation.children) scan_operation(child);
    }

    void collect_initial_instances()
    {
        for (const auto& [identity, declaration] : classes_) {
            if (declaration->generic_parameters.empty()) {
                add_instance(Type{TypeKind::Unknown, false, false, identity});
            }
        }
        for (const auto& declaration : program_.declarations) {
            for (const auto& parameter : declaration.parameters) {
                scan_type(parameter.type.type);
            }
            for (const auto& field : declaration.fields) scan_type(field.type.type);
            for (const auto& variant : declaration.variants) {
                if (variant.payload_type) scan_type(variant.payload_type->type);
            }
            if (declaration.return_type) scan_type(declaration.return_type->type);
            for (const auto& related : declaration.related_types) {
                scan_type(related.type);
            }
            if (declaration.body) scan_operation(*declaration.body);
        }
        for (const auto& operation : program_.top_level_operations) {
            scan_operation(operation);
        }
    }

    void collect_fields(
        const Type& instance,
        const IRDeclaration& declaration,
        std::vector<FieldView>& output,
        std::set<std::string>& visiting)
    {
        const auto key = type_key(instance, false);
        if (!visiting.insert(key).second) return;
        const auto substitutions = substitutions_for(declaration, instance);
        for (const auto& related : declaration.related_types) {
            Type base = substitute(related.type, substitutions);
            const auto found = classes_.find(base.name);
            if (found == classes_.end() || !is_concrete(base)) continue;
            add_instance(base);
            collect_fields(nonnullable(base), *found->second, output, visiting);
        }
        for (const auto& field : declaration.fields) {
            output.push_back(FieldView{&field, substitute(field.type.type, substitutions)});
        }
        visiting.erase(key);
    }

    template <typename TargetCallback>
    void collect_storage_targets(
        const Type& storage_type,
        const std::string& path,
        std::set<std::string>& visiting_values,
        TargetCallback&& callback)
    {
        Type type = nonnullable(storage_type);
        if (classes_.contains(type.name)) {
            if (is_concrete(type)) callback(type, path);
            return;
        }
        if (type.name == "List" || type.name == "Array") {
            if (!type.arguments.empty()) {
                collect_storage_targets(type.arguments.front(), path + "[]",
                    visiting_values, callback);
            }
            return;
        }
        if (type.name == "Map") {
            if (type.arguments.size() >= 2) {
                collect_storage_targets(type.arguments[1], path + "[value]",
                    visiting_values, callback);
            }
            return;
        }
        if (type.name == "Result") {
            if (!type.arguments.empty()) {
                collect_storage_targets(type.arguments[0], path + "::ok",
                    visiting_values, callback);
            }
            if (type.arguments.size() >= 2) {
                collect_storage_targets(type.arguments[1], path + "::error",
                    visiting_values, callback);
            }
            return;
        }

        const auto enum_found = enums_.find(type.name);
        if (enum_found != enums_.end()) {
            const auto key = "enum:" + type_key(type, false);
            if (!visiting_values.insert(key).second) return;
            const auto substitutions = substitutions_for(*enum_found->second, type);
            for (const auto& variant : enum_found->second->variants) {
                if (!variant.payload_type) continue;
                collect_storage_targets(
                    substitute(variant.payload_type->type, substitutions),
                    path + "::" + variant.name,
                    visiting_values, callback);
            }
            visiting_values.erase(key);
            return;
        }

        const auto struct_found = structs_.find(type.name);
        if (struct_found == structs_.end()) return;
        const auto key = "struct:" + type_key(type, false);
        if (!visiting_values.insert(key).second) return;
        const auto substitutions = substitutions_for(*struct_found->second, type);
        for (const auto& field : struct_found->second->fields) {
            collect_storage_targets(substitute(field.type.type, substitutions),
                path + '.' + field.name, visiting_values, callback);
        }
        visiting_values.erase(key);
    }

    void close_instances()
    {
        bool changed = true;
        while (changed) {
            changed = false;
            std::vector<Type> snapshot;
            for (const auto& [key, instance] : instances_) {
                static_cast<void>(key);
                snapshot.push_back(instance);
            }
            for (const auto& instance : snapshot) {
                const auto declaration = classes_.find(instance.name);
                if (declaration == classes_.end()) continue;
                std::vector<FieldView> fields;
                std::set<std::string> visiting_bases;
                collect_fields(instance, *declaration->second, fields, visiting_bases);
                for (const auto& view : fields) {
                    if (view.field->is_weak) continue;
                    std::set<std::string> visiting_values;
                    collect_storage_targets(view.type, view.field->name,
                        visiting_values,
                        [&](const Type& target, const std::string&) {
                            changed = add_instance(target) || changed;
                        });
                }
            }
        }
    }

    void build_edges()
    {
        for (const auto& [owner_key, instance] : instances_) {
            const auto declaration = classes_.find(instance.name);
            if (declaration == classes_.end()) continue;
            std::vector<FieldView> fields;
            std::set<std::string> visiting_bases;
            collect_fields(instance, *declaration->second, fields, visiting_bases);
            for (const auto& view : fields) {
                if (view.field->is_weak) continue;
                std::set<std::string> visiting_values;
                collect_storage_targets(view.type, view.field->name,
                    visiting_values,
                    [&](const Type& target, const std::string& path) {
                        const auto target_key = type_key(nonnullable(target), false);
                        if (!instances_.contains(target_key)) return;
                        program_.strong_ownership_edges.push_back(
                            IRStrongOwnershipEdge{
                                owner_key,
                                display(instance),
                                path,
                                target_key,
                                display(nonnullable(target)),
                                view.field->origin,
                                view.field->default_value.has_value()
                                    || view.type.nullable
                                    || view.type.name == "List"
                                    || view.type.name == "Array"
                                    || view.type.name == "Map",
                            });
                    });
            }
        }
        std::ranges::sort(program_.strong_ownership_edges,
            [](const auto& left, const auto& right) {
                return std::tie(left.owner_type, left.target_type,
                           left.field_path, left.origin.module_name,
                           left.origin.location.line, left.origin.location.column)
                    < std::tie(right.owner_type, right.target_type,
                          right.field_path, right.origin.module_name,
                          right.origin.location.line, right.origin.location.column);
            });
        program_.strong_ownership_edges.erase(
            std::unique(program_.strong_ownership_edges.begin(),
                program_.strong_ownership_edges.end(),
                [](const auto& left, const auto& right) {
                    return left.owner_type == right.owner_type
                        && left.target_type == right.target_type
                        && left.field_path == right.field_path
                        && left.origin.module_name == right.origin.module_name
                        && left.origin.location.line
                            == right.origin.location.line
                        && left.origin.location.column
                            == right.origin.location.column;
                }),
            program_.strong_ownership_edges.end());
    }

    std::vector<std::size_t> cycle_path(
        const std::vector<std::string>& component,
        const std::map<std::string, std::vector<std::size_t>>& adjacency) const
    {
        const std::set<std::string> allowed(component.begin(), component.end());
        const std::string& start = component.front();
        std::set<std::string> visiting{start};
        std::vector<std::size_t> path;
        std::function<bool(const std::string&, bool)> search =
            [&](const std::string& current, bool has_deferred_edge) {
                const auto found = adjacency.find(current);
                if (found == adjacency.end()) return false;
                for (const auto edge_index : found->second) {
                    const auto& edge = program_.strong_ownership_edges[edge_index];
                    if (!allowed.contains(edge.target_type)) continue;
                    path.push_back(edge_index);
                    const bool path_can_form = has_deferred_edge
                        || edge.target_may_be_deferred;
                    if (edge.target_type == start && path_can_form) return true;
                    if (visiting.insert(edge.target_type).second) {
                        if (search(edge.target_type, path_can_form)) return true;
                        visiting.erase(edge.target_type);
                    }
                    path.pop_back();
                }
                return false;
            };
        if (!search(start, false)) return {};
        return path;
    }

    std::string cycle_text(const std::vector<IRStrongOwnershipEdge>& edges) const
    {
        if (edges.empty()) return {};
        std::string result = "cycle: " + edges.front().owner_display + '.'
            + edges.front().field_path;
        for (std::size_t index = 0; index < edges.size(); ++index) {
            result += " -> " + edges[index].target_display;
            if (index + 1 < edges.size()) {
                result += '.' + edges[index + 1].field_path;
            }
        }
        return result;
    }

    void add_cycle(const std::vector<std::size_t>& path)
    {
        IRStrongReferenceCycle cycle;
        for (const auto index : path) {
            cycle.edges.push_back(program_.strong_ownership_edges[index]);
        }
        if (cycle.edges.empty()) return;

        Diagnostic diagnostic;
        diagnostic.code = "strong-reference-cycle";
        diagnostic.message =
            "possible strong reference cycle may keep objects alive";
        diagnostic.source = diagnostic_source(cycle.edges.front().origin);
        for (const auto& edge : cycle.edges) {
            diagnostic.notes.push_back(DiagnosticNote{
                edge.owner_display + '.' + edge.field_path + " -> "
                    + edge.target_display,
                diagnostic_source(edge.origin),
            });
        }
        diagnostic.notes.push_back(DiagnosticNote{cycle_text(cycle.edges), std::nullopt});
        diagnostic.notes.push_back(DiagnosticNote{
            "ARC cannot reclaim objects whose strong references keep the cycle alive",
            std::nullopt,
        });
        diagnostic.notes.push_back(DiagnosticNote{
            "consider whether one non-owning back-reference should be weak",
            std::nullopt,
        });
        program_.strong_reference_cycles.push_back(std::move(cycle));
        program_.diagnostics.push_back(std::move(diagnostic));
    }

    void detect_cycles()
    {
        std::map<std::string, std::vector<std::size_t>> adjacency;
        for (const auto& [key, instance] : instances_) {
            static_cast<void>(instance);
            adjacency[key];
        }
        for (std::size_t index = 0;
             index < program_.strong_ownership_edges.size(); ++index) {
            adjacency[program_.strong_ownership_edges[index].owner_type]
                .push_back(index);
        }

        std::map<std::string, std::size_t> indices;
        std::map<std::string, std::size_t> lowlinks;
        std::set<std::string> on_stack;
        std::vector<std::string> stack;
        std::vector<std::vector<std::string>> components;
        std::size_t next_index = 0;

        std::function<void(const std::string&)> visit =
            [&](const std::string& node) {
                indices[node] = next_index;
                lowlinks[node] = next_index;
                ++next_index;
                stack.push_back(node);
                on_stack.insert(node);

                for (const auto edge_index : adjacency[node]) {
                    const auto& target =
                        program_.strong_ownership_edges[edge_index].target_type;
                    if (!indices.contains(target)) {
                        visit(target);
                        lowlinks[node] = std::min(lowlinks[node], lowlinks[target]);
                    } else if (on_stack.contains(target)) {
                        lowlinks[node] = std::min(lowlinks[node], indices[target]);
                    }
                }

                if (lowlinks[node] != indices[node]) return;
                std::vector<std::string> component;
                while (!stack.empty()) {
                    auto member = std::move(stack.back());
                    stack.pop_back();
                    on_stack.erase(member);
                    component.push_back(std::move(member));
                    if (component.back() == node) break;
                }
                std::ranges::sort(component);
                const bool self_cycle = component.size() == 1
                    && std::ranges::any_of(adjacency[component.front()],
                        [&](const auto edge_index) {
                            return program_.strong_ownership_edges[edge_index]
                                .target_type == component.front();
                        });
                const bool can_form = std::ranges::any_of(component,
                    [&](const auto& owner) {
                        return std::ranges::any_of(adjacency[owner],
                            [&](const auto edge_index) {
                                const auto& edge =
                                    program_.strong_ownership_edges[edge_index];
                                return std::ranges::binary_search(
                                           component, edge.target_type)
                                    && edge.target_may_be_deferred;
                            });
                    });
                if ((component.size() > 1 || self_cycle) && can_form) {
                    components.push_back(std::move(component));
                }
            };

        for (const auto& [node, edges] : adjacency) {
            static_cast<void>(edges);
            if (!indices.contains(node)) visit(node);
        }
        std::ranges::sort(components,
            [](const auto& left, const auto& right) {
                return left.front() < right.front();
            });
        for (const auto& component : components) {
            const auto path = cycle_path(component, adjacency);
            if (!path.empty()) add_cycle(path);
        }
    }

    IRProgram& program_;
    std::map<std::string, const IRDeclaration*> declarations_;
    std::map<std::string, const IRDeclaration*> classes_;
    std::map<std::string, const IRDeclaration*> structs_;
    std::map<std::string, const IRDeclaration*> enums_;
    std::map<std::string, Type> instances_;
    std::map<std::string, std::size_t> source_name_counts_;
};

} // namespace

void StrongCycleAnalyzer::analyze(IRProgram& program) const
{
    Analysis(program).run();
}

} // namespace toro
