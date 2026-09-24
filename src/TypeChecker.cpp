#include "toro/TypeChecker.hpp"

#include <algorithm>
#include <functional>
#include <stdexcept>
#include <string>

namespace toro {
namespace {

const Type int_type{TypeKind::Int};
const Type dec_type{TypeKind::Dec};
const Type string_type{TypeKind::String};
const Type bool_type{TypeKind::Bool};
const Type null_type{TypeKind::Null};
const Type void_type{TypeKind::Void};
const Type unknown_type{TypeKind::Unknown, false, true};

[[noreturn]] void throw_type_error(SourceLocation location, const std::string& message)
{
    throw std::runtime_error(
        "line " + std::to_string(location.line) + ", column "
        + std::to_string(location.column) + ": type error: " + message);
}

SourceLocation token_location(const Token& token)
{
    return SourceLocation{token.line, token.column};
}

void reject_standalone_null_type(Type type, SourceLocation location)
{
    if (type.kind == TypeKind::Null) {
        throw_type_error(location, "null cannot be used as a standalone declared type");
    }
}

bool is_directly_assignable(const Type& expected, const Type& actual)
{
    if (actual.kind == TypeKind::Null) {
        return expected.nullable;
    }
    if (actual.nullable && !expected.nullable) {
        return false;
    }
    if (is_unknown(expected) || is_unknown(actual)) {
        if (is_unknown(expected) && is_unknown(actual)) {
            return expected.name.empty() || actual.name.empty()
                || (expected.name == actual.name
                    && expected.arguments == actual.arguments);
        }
        const Type& unknown = is_unknown(expected) ? expected : actual;
        return unknown.name.empty();
    }
    return expected.kind == actual.kind;
}

bool has_same_base_type(const Type& left, const Type& right)
{
    if (is_unknown(left) || is_unknown(right)) {
        if (is_unknown(left) && is_unknown(right)) {
            return left.name.empty() || right.name.empty()
                || (left.name == right.name && left.arguments == right.arguments);
        }
        const Type& unknown = is_unknown(left) ? left : right;
        return unknown.name.empty();
    }
    return left.kind == right.kind;
}

bool has_zero_value(const Type& type)
{
    if (type.nullable) {
        return true;
    }
    switch (type.kind) {
    case TypeKind::Int:
    case TypeKind::Dec:
    case TypeKind::String:
    case TypeKind::Bool:
        return true;
    case TypeKind::Null:
    case TypeKind::Void:
    case TypeKind::Unknown:
        return false;
    }
    return false;
}

std::string format_type_reference(const TypeReference& reference)
{
    std::string result = reference.name;
    if (!reference.arguments.empty()) {
        result += '<';
        for (std::size_t index = 0; index < reference.arguments.size(); ++index) {
            if (index != 0) {
                result += ", ";
            }
            result += format_type_reference(reference.arguments[index]);
        }
        result += '>';
    }
    if (reference.nullable) {
        result += '?';
    }
    return result;
}

} // namespace

void TypeChecker::check(const Program& program)
{
    scopes_.clear();
    generic_parameter_scopes_.clear();
    current_return_type_.reset();
    current_type_name_.reset();
    current_location_ = SourceLocation{1, 1};
    push_scope();
    predeclare(program.statements);
    validate_nominal_types();
    for (const auto& statement : program.statements) {
        check_statement(*statement);
    }
    pop_scope();
}

void TypeChecker::check_statement_list(
    const std::vector<std::unique_ptr<Stmt>>& statements)
{
    predeclare(statements);
    for (const auto& statement : statements) {
        check_statement(*statement);
    }
}

void TypeChecker::predeclare(const std::vector<std::unique_ptr<Stmt>>& statements)
{
    for (const auto& statement : statements) {
        if (statement->kind == StmtKind::FunctionDeclaration) {
            const auto& function = static_cast<const FunctionDeclarationStmt&>(*statement);
            push_generic_parameters(function.generic_parameters);
            FunctionSignature signature{
                {},
                function.return_type ? resolve_type(*function.return_type) : void_type,
                function.return_type,
                function.generic_parameters,
                {},
                function.location,
            };
            signature.parameters.reserve(function.parameters.size());
            for (const auto& parameter : function.parameters) {
                signature.parameters.push_back(FunctionParameterType{
                    parameter.name,
                    resolve_type(parameter.type),
                    parameter.type,
                });
            }
            pop_generic_parameters();
            auto& overloads = scopes_.back().functions[function.name];
            if (std::any_of(overloads.begin(), overloads.end(),
                    [&](const FunctionSignature& candidate) {
                        return parameter_types_match(candidate, signature);
                    })) {
                throw_type_error(function.location,
                    "duplicate callable signature for function '" + function.name + "'");
            }
            overloads.push_back(std::move(signature));
            continue;
        }

        switch (statement->kind) {
        case StmtKind::StructDeclaration: {
            const auto& declaration = static_cast<const StructDeclarationStmt&>(*statement);
            declare_value(declaration.name,
                Type{TypeKind::Unknown, false, false, declaration.name});
            push_generic_parameters(declaration.generic_parameters);
            NominalTypeInfo type_info;
            type_info.kind = NominalKind::Struct;
            type_info.location = declaration.location;
            type_info.generic_parameters = declaration.generic_parameters;
            for (const auto& interface_name : declaration.interfaces) {
                type_info.interfaces.push_back(interface_name.name);
            }
            for (const auto& field : declaration.fields) {
                const Type field_type = resolve_type(field.type);
                type_info.field_order.push_back(field.name);
                type_info.fields.emplace(field.name, FieldInfo{
                    field_type,
                    Visibility::Public,
                    field.default_value == nullptr && !has_zero_value(field_type),
                    field.default_value != nullptr,
                    declaration.name,
                    field.type,
                });
            }
            for (const auto& method : declaration.methods) {
                const auto& method_declaration =
                    static_cast<const MethodDeclaration&>(*method);
                push_generic_parameters(method_declaration.generic_parameters);
                FunctionSignature signature{
                    {},
                    method_declaration.return_type
                        ? resolve_type(*method_declaration.return_type) : void_type,
                    method_declaration.return_type,
                    method_declaration.generic_parameters,
                    {},
                    method_declaration.location,
                };
                for (const auto& parameter : method_declaration.parameters) {
                    signature.parameters.push_back(FunctionParameterType{
                        parameter.name, resolve_type(parameter.type), parameter.type});
                }
                pop_generic_parameters();
                auto& overloads = type_info.methods[method_declaration.name];
                if (std::any_of(overloads.begin(), overloads.end(),
                        [&](const MethodInfo& candidate) {
                            return parameter_types_match(candidate.signature, signature);
                        })) {
                    throw_type_error(method_declaration.location,
                        "duplicate callable signature for method '" + declaration.name
                            + "." + method_declaration.name + "'");
                }
                overloads.push_back(MethodInfo{
                    std::move(signature), Visibility::Public, declaration.name,
                    false, false, false});
            }
            for (const auto& conversion : declaration.conversions) {
                declare_conversion(declaration.name, resolve_type(conversion->target_type));
            }
            scopes_.back().nominal_types.emplace(declaration.name, std::move(type_info));
            pop_generic_parameters();
            break;
        }
        case StmtKind::ClassDeclaration: {
            const auto& declaration = static_cast<const ClassDeclarationStmt&>(*statement);
            declare_value(declaration.name,
                Type{TypeKind::Unknown, false, false, declaration.name});
            push_generic_parameters(declaration.generic_parameters);
            NominalTypeInfo type_info;
            type_info.kind = NominalKind::Class;
            type_info.is_abstract = declaration.is_abstract;
            type_info.location = declaration.location;
            type_info.generic_parameters = declaration.generic_parameters;
            if (declaration.base_type) {
                type_info.base = declaration.base_type->name;
            }
            for (const auto& interface_name : declaration.interfaces) {
                type_info.interfaces.push_back(interface_name.name);
            }
            for (const auto& member : declaration.members) {
                if (member->kind == ClassMemberKind::Field) {
                    const auto& field = static_cast<const ClassField&>(*member);
                    const Type field_type = resolve_type(field.type);
                    type_info.field_order.push_back(field.name);
                    type_info.fields.emplace(field.name, FieldInfo{
                        field_type,
                        field.visibility,
                        field.default_value == nullptr && !has_zero_value(field_type),
                        field.default_value != nullptr,
                        declaration.name,
                        field.type,
                    });
                } else if (member->kind == ClassMemberKind::Method) {
                    const auto& method = static_cast<const MethodDeclaration&>(*member);
                    push_generic_parameters(method.generic_parameters);
                    FunctionSignature signature{
                        {},
                        method.return_type ? resolve_type(*method.return_type) : void_type,
                        method.return_type,
                        method.generic_parameters,
                        {},
                        method.location,
                    };
                    signature.parameters.reserve(method.parameters.size());
                    for (const auto& parameter : method.parameters) {
                        signature.parameters.push_back(FunctionParameterType{
                            parameter.name,
                            resolve_type(parameter.type),
                            parameter.type,
                        });
                    }
                    pop_generic_parameters();
                    auto& overloads = type_info.methods[method.name];
                    if (std::any_of(overloads.begin(), overloads.end(),
                            [&](const MethodInfo& candidate) {
                                return parameter_types_match(candidate.signature, signature);
                            })) {
                        throw_type_error(method.location,
                            "duplicate callable signature for method '" + declaration.name
                                + "." + method.name + "'");
                    }
                    overloads.push_back(MethodInfo{
                        std::move(signature), method.visibility, declaration.name,
                        method.is_virtual || method.is_override,
                        method.body == nullptr, method.is_override});
                } else {
                    const auto& conversion = static_cast<const ConversionOverload&>(*member);
                    declare_conversion(
                        declaration.name, resolve_type(conversion.target_type));
                }
            }
            scopes_.back().nominal_types.emplace(declaration.name, std::move(type_info));
            pop_generic_parameters();
            break;
        }
        case StmtKind::InterfaceDeclaration: {
            const auto& declaration = static_cast<const InterfaceDeclarationStmt&>(*statement);
            declare_value(declaration.name,
                Type{TypeKind::Unknown, false, false, declaration.name});
            push_generic_parameters(declaration.generic_parameters);
            NominalTypeInfo type_info;
            type_info.kind = NominalKind::Interface;
            type_info.is_abstract = true;
            type_info.location = declaration.location;
            type_info.generic_parameters = declaration.generic_parameters;
            for (const auto& method : declaration.methods) {
                push_generic_parameters(method.generic_parameters);
                FunctionSignature signature{
                    {},
                    method.return_type ? resolve_type(*method.return_type) : void_type,
                    method.return_type,
                    method.generic_parameters,
                    {},
                    method.location,
                };
                for (const auto& parameter : method.parameters) {
                    signature.parameters.push_back(FunctionParameterType{
                        parameter.name, resolve_type(parameter.type), parameter.type});
                }
                pop_generic_parameters();
                auto& overloads = type_info.methods[method.name];
                if (std::any_of(overloads.begin(), overloads.end(),
                        [&](const MethodInfo& candidate) {
                            return parameter_types_match(candidate.signature, signature);
                        })) {
                    throw_type_error(method.location,
                        "duplicate callable signature for interface method '"
                            + declaration.name + "." + method.name + "'");
                }
                overloads.push_back(MethodInfo{
                    std::move(signature), Visibility::Public, declaration.name,
                    false, true, false});
            }
            scopes_.back().nominal_types.emplace(declaration.name, std::move(type_info));
            pop_generic_parameters();
            break;
        }
        case StmtKind::EnumDeclaration: {
            const auto& declaration = static_cast<const EnumDeclarationStmt&>(*statement);
            declare_value(declaration.name,
                Type{TypeKind::Unknown, false, false, declaration.name});
            NominalTypeInfo type_info;
            type_info.kind = NominalKind::Enum;
            type_info.location = declaration.location;
            for (const auto& variant : declaration.variants) {
                const auto [unused, inserted] = type_info.variants.emplace(
                    variant.name,
                    EnumVariantInfo{
                        variant.payload_type
                            ? std::optional<Type>{resolve_type(*variant.payload_type)}
                            : std::nullopt,
                        variant.location,
                    });
                static_cast<void>(unused);
                if (!inserted) {
                    throw_type_error(
                        variant.location, "duplicate enum variant '"
                            + declaration.name + "." + variant.name + "'");
                }
                type_info.variant_order.push_back(variant.name);
            }
            scopes_.back().nominal_types.emplace(
                declaration.name, std::move(type_info));
            break;
        }
        default:
            break;
        }
    }
}

void TypeChecker::validate_nominal_types()
{
    for (const auto& [name, type_info] : scopes_.back().nominal_types) {
        if (type_info.base) {
            const auto* base = find_nominal_type(*type_info.base);
            if (!base || base->kind != NominalKind::Class) {
                throw_type_error(type_info.location,
                    "base type '" + *type_info.base + "' of class '" + name
                        + "' must name an existing class");
            }
        }
        for (const auto& interface_name : type_info.interfaces) {
            const auto* interface_type = find_nominal_type(interface_name);
            if (!interface_type || interface_type->kind != NominalKind::Interface) {
                throw_type_error(type_info.location,
                    "implemented type '" + interface_name + "' on '" + name
                        + "' must name an existing interface");
            }
        }
    }

    validate_inheritance_cycles();
    for (const auto& [name, type_info] : scopes_.back().nominal_types) {
        if (type_info.kind == NominalKind::Class) {
            validate_class(name, type_info);
        }
        if (type_info.kind != NominalKind::Interface) {
            validate_interfaces(name, type_info);
        }
    }
}

void TypeChecker::validate_inheritance_cycles() const
{
    std::unordered_map<std::string, int> state;
    std::function<void(const std::string&)> visit = [&](const std::string& name) {
        if (state[name] == 2) {
            return;
        }
        if (state[name] == 1) {
            const auto* type_info = find_nominal_type(name);
            throw_type_error(type_info ? type_info->location : SourceLocation{1, 1},
                "inheritance cycle involving class '" + name + "'");
        }
        state[name] = 1;
        const auto* type_info = find_nominal_type(name);
        if (type_info && type_info->base) {
            visit(*type_info->base);
        }
        state[name] = 2;
    };

    for (const auto& [name, type_info] : scopes_.back().nominal_types) {
        if (type_info.kind == NominalKind::Class) {
            visit(name);
        }
    }
}

void TypeChecker::validate_class(
    const std::string& name,
    const NominalTypeInfo& type_info) const
{
    for (const auto& [method_name, overloads] : type_info.methods) {
        const auto inherited = type_info.base
            ? find_methods(*type_info.base, method_name)
            : std::vector<const MethodInfo*>{};
        for (const auto& method : overloads) {
            const auto matching = std::find_if(
                inherited.begin(), inherited.end(), [&](const MethodInfo* candidate) {
                    return parameter_types_match(
                        method.signature, candidate->signature);
                });
            if (method.is_override && inherited.empty()) {
                throw_type_error(type_info.location,
                    "method '" + name + "." + method_name
                        + "' is marked override but no inherited method exists");
            }
            if (method.is_override && matching == inherited.end()) {
                throw_type_error(type_info.location,
                    "override '" + name + "." + method_name
                        + "' does not match the inherited signature");
            }
            if (method.is_override && !(*matching)->is_virtual) {
                throw_type_error(type_info.location,
                    "method '" + name + "." + method_name
                        + "' cannot override non-virtual method");
            }
            if (method.is_override
                && !signatures_match(method.signature, (*matching)->signature)) {
                throw_type_error(type_info.location,
                    "override '" + name + "." + method_name
                        + "' does not match the inherited signature");
            }
            if (!method.is_override && matching != inherited.end()
                && (*matching)->is_virtual) {
                throw_type_error(type_info.location,
                    "method '" + name + "." + method_name
                        + "' must use override for an inherited virtual method");
            }
        }
    }

    if (!type_info.is_abstract) {
        std::unordered_set<std::string> candidate_names;
        const NominalTypeInfo* current = &type_info;
        while (current) {
            for (const auto& [method_name, unused] : current->methods) {
                static_cast<void>(unused);
                candidate_names.insert(method_name);
            }
            current = current->base ? find_nominal_type(*current->base) : nullptr;
        }
        for (const auto& method_name : candidate_names) {
            for (const auto* method : find_methods(name, method_name)) {
                if (method->is_abstract) {
                    throw_type_error(type_info.location,
                        "concrete class '" + name
                            + "' does not implement abstract method '"
                            + method_name + "'");
                }
            }
        }
    }
}

void TypeChecker::validate_interfaces(
    const std::string& name,
    const NominalTypeInfo& type_info) const
{
    for (const auto& interface_name : type_info.interfaces) {
        const auto* interface_type = find_nominal_type(interface_name);
        if (!interface_type) {
            continue;
        }
        for (const auto& [method_name, requirements] : interface_type->methods) {
            const auto implementations = find_methods(name, method_name);
            for (const auto& requirement : requirements) {
                const auto implementation = std::find_if(
                    implementations.begin(), implementations.end(),
                    [&](const MethodInfo* candidate) {
                        return parameter_types_match(
                            candidate->signature, requirement.signature);
                    });
                if (implementation == implementations.end()
                    || (*implementation)->visibility != Visibility::Public) {
                    throw_type_error(type_info.location,
                        "type '" + name
                            + "' does not provide public interface method '"
                            + interface_name + "." + method_name + "'");
                }
                if (!signatures_match(
                        (*implementation)->signature, requirement.signature)) {
                    throw_type_error(type_info.location,
                        "method '" + name + "." + method_name
                            + "' does not match interface '" + interface_name + "'");
                }
            }
        }
    }
}

void TypeChecker::check_statement(const Stmt& statement)
{
    current_location_ = statement.location;
    switch (statement.kind) {
    case StmtKind::VariableDeclaration: {
        const auto& declaration = static_cast<const VariableDeclarationStmt&>(statement);
        if (declaration.explicit_type) {
            const Type declared = resolve_type(*declaration.explicit_type);
            reject_standalone_null_type(declared, declaration.explicit_type->location);
            const Type initializer = require_value(
                check_expression(*declaration.initializer, declared),
                declaration.location);
            require_assignable(declared, initializer, declaration.location);
            declare_value(declaration.name, declared);
        } else {
            const Type initializer = require_value(
                check_expression(*declaration.initializer), declaration.location);
            if (initializer.kind == TypeKind::Null) {
                throw_type_error(
                    declaration.location,
                    "cannot infer a variable type from null without nullable types");
            }
            declare_value(declaration.name, initializer);
        }
        return;
    }
    case StmtKind::Assignment: {
        const auto& assignment = static_cast<const AssignmentStmt&>(statement);
        const auto expected = find_value(assignment.name);
        const Type actual = require_value(
            check_expression(*assignment.value, expected), assignment.location);
        if (expected) {
            require_assignable(*expected, actual, assignment.location);
        }
        return;
    }
    case StmtKind::MemberAssignment: {
        const auto& assignment = static_cast<const MemberAssignmentStmt&>(statement);
        const Type expected = check_member_access(*assignment.target);
        const Type value = require_value(
            check_expression(*assignment.value, expected), assignment.location);
        require_assignable(expected, value, assignment.location);
        return;
    }
    case StmtKind::Expression:
        static_cast<void>(check_expression(
            *static_cast<const ExpressionStmt&>(statement).expression));
        return;
    case StmtKind::FunctionDeclaration:
        check_function(static_cast<const FunctionDeclarationStmt&>(statement));
        return;
    case StmtKind::Return: {
        const auto& return_statement = static_cast<const ReturnStmt&>(statement);
        if (!current_return_type_) {
            throw_type_error(statement.location, "return is only valid inside a function");
        }
        if (!return_statement.value) {
            if (current_return_type_->kind != TypeKind::Void) {
                throw_type_error(
                    statement.location,
                    "return requires a value of type '"
                        + std::string(type_name(*current_return_type_)) + "'");
            }
            return;
        }
        if (current_return_type_->kind == TypeKind::Void) {
            throw_type_error(
                statement.location, "a function without a return type cannot return a value");
        }
        const Type actual = require_value(
            check_expression(*return_statement.value, *current_return_type_),
            statement.location);
        require_assignable(*current_return_type_, actual, statement.location);
        return;
    }
    case StmtKind::Block:
        check_block(static_cast<const BlockStmt&>(statement));
        return;
    case StmtKind::If: {
        const auto& if_statement = static_cast<const IfStmt&>(statement);
        require_condition(check_expression(*if_statement.condition), statement.location);
        check_block(*if_statement.then_block);
        if (if_statement.else_branch) {
            check_statement(*if_statement.else_branch);
        }
        return;
    }
    case StmtKind::While: {
        const auto& loop = static_cast<const WhileStmt&>(statement);
        require_condition(check_expression(*loop.condition), statement.location);
        check_block(*loop.body);
        return;
    }
    case StmtKind::ForIn: {
        const auto& loop = static_cast<const ForInStmt&>(statement);
        static_cast<void>(require_value(check_expression(*loop.collection), statement.location));
        push_scope();
        declare_value(loop.variable_name, unknown_type);
        check_statement_list(loop.body->statements);
        pop_scope();
        return;
    }
    case StmtKind::Stop:
    case StmtKind::Continue:
    case StmtKind::EnumDeclaration:
    case StmtKind::InterfaceDeclaration:
        return;
    case StmtKind::Handle: {
        const auto& handle = static_cast<const HandleStmt&>(statement);
        const Type handled = require_value(
            check_expression(*handle.expression), statement.location);
        std::vector<std::string> variant_order;
        std::unordered_map<std::string, std::optional<Type>> variants;
        if (!handled.nullable && is_unknown(handled) && !handled.name.empty()) {
            if (const auto* type_info = find_nominal_type(handled.name);
                type_info && type_info->kind == NominalKind::Enum) {
                variant_order = type_info->variant_order;
                for (const auto& [name, variant] : type_info->variants) {
                    variants.emplace(name, variant.payload_type);
                }
            } else if (handled.name == "Result" && handled.arguments.size() == 2) {
                variant_order = {"ok", "error"};
                variants.emplace("ok", handled.arguments[0]);
                variants.emplace("error", handled.arguments[1]);
            }
        }
        if (variants.empty()) {
            throw_type_error(
                statement.location, "handle expression must have an enum type, got '"
                    + type_name(handled) + "'");
        }

        std::unordered_set<std::string> covered;
        for (const auto& handle_case : handle.cases) {
            current_location_ = handle_case.location;
            const auto variant = variants.find(handle_case.variant_name);
            if (variant == variants.end()) {
                throw_type_error(
                    handle_case.location, "enum '" + type_name(handled)
                        + "' has no variant named '" + handle_case.variant_name + "'");
            }
            if (!covered.insert(handle_case.variant_name).second) {
                throw_type_error(
                    handle_case.location, "duplicate handle case '"
                        + handle_case.variant_name + "'");
            }
            if (variant->second && !handle_case.binding_name) {
                throw_type_error(
                    handle_case.location, "handle case '" + handle_case.variant_name
                        + "' requires a payload binding");
            }
            if (!variant->second && handle_case.binding_name) {
                throw_type_error(
                    handle_case.location, "handle case '" + handle_case.variant_name
                        + "' cannot bind a payload");
            }
            push_scope();
            if (handle_case.binding_name) {
                declare_value(*handle_case.binding_name, *variant->second);
            }
            check_statement_list(handle_case.body->statements);
            pop_scope();
        }
        std::vector<std::string> missing;
        for (const auto& variant_name : variant_order) {
            if (!covered.contains(variant_name)) {
                missing.push_back(variant_name);
            }
        }
        if (!missing.empty()) {
            std::string message = "non-exhaustive handle for '" + type_name(handled)
                + "'; missing variants: ";
            for (std::size_t index = 0; index < missing.size(); ++index) {
                if (index != 0) {
                    message += ", ";
                }
                message += missing[index];
            }
            throw_type_error(statement.location, message);
        }
        return;
    }
    case StmtKind::StructDeclaration: {
        const auto& declaration = static_cast<const StructDeclarationStmt&>(statement);
        push_generic_parameters(declaration.generic_parameters);
        for (const auto& field : declaration.fields) {
            const Type declared = resolve_type(field.type);
            reject_standalone_null_type(declared, field.type.location);
            if (field.default_value) {
                current_location_ = field.location;
                const Type actual = require_value(
                    check_expression(*field.default_value, declared), field.location);
                require_assignable(declared, actual, field.location);
            }
        }
        const Type source_type{TypeKind::Unknown, false, false, declaration.name};
        for (const auto& method : declaration.methods) {
            check_method(static_cast<const MethodDeclaration&>(*method), source_type);
        }
        for (const auto& conversion : declaration.conversions) {
            check_conversion(*conversion, source_type);
        }
        pop_generic_parameters();
        return;
    }
    case StmtKind::ClassDeclaration: {
        const auto& declaration = static_cast<const ClassDeclarationStmt&>(statement);
        push_generic_parameters(declaration.generic_parameters);
        for (const auto& member : declaration.members) {
            current_location_ = member->location;
            if (member->kind == ClassMemberKind::Field) {
                const auto& field = static_cast<const ClassField&>(*member);
                const Type declared = resolve_type(field.type);
                reject_standalone_null_type(declared, field.type.location);
                if (field.is_weak) {
                    const auto* target = find_nominal_type(declared.name);
                    if (!declared.nullable || declared.kind != TypeKind::Unknown
                        || !target || target->kind != NominalKind::Class) {
                        throw_type_error(
                            field.location,
                            "weak fields require a nullable class type");
                    }
                }
                if (field.default_value) {
                    const Type actual = require_value(
                        check_expression(*field.default_value, declared), field.location);
                    require_assignable(declared, actual, field.location);
                }
            } else if (member->kind == ClassMemberKind::Method) {
                check_method(
                    static_cast<const MethodDeclaration&>(*member),
                    Type{TypeKind::Unknown, false, false, declaration.name});
            } else {
                check_conversion(
                    static_cast<const ConversionOverload&>(*member),
                    Type{TypeKind::Unknown, false, false, declaration.name});
            }
        }
        pop_generic_parameters();
        return;
    }
    }
}

Type TypeChecker::check_expression(
    const Expr& expression,
    std::optional<Type> expected_type)
{
    switch (expression.kind) {
    case ExprKind::Integer:
        return int_type;
    case ExprKind::Decimal:
        return dec_type;
    case ExprKind::String:
        return string_type;
    case ExprKind::Bool:
        return bool_type;
    case ExprKind::Null:
        return null_type;
    case ExprKind::Identifier: {
        const auto& identifier = static_cast<const IdentifierExpr&>(expression);
        if (const auto type = find_value(identifier.name)) {
            return *type;
        }
        if (!find_functions(identifier.name).empty() || identifier.name == "print") {
            return unknown_type;
        }
        return unknown_type;
    }
    case ExprKind::Unary: {
        const auto& unary = static_cast<const UnaryExpr&>(expression);
        const Type operand = require_value(
            check_expression(*unary.operand), token_location(unary.operator_token));
        if (operand.deferred && !operand.name.empty()) {
            throw_type_error(
                token_location(unary.operator_token),
                "unary '-' cannot use unconstrained generic type '"
                    + type_name(operand) + "'");
        }
        if (operand.nullable) {
            throw_type_error(
                token_location(unary.operator_token),
                "unary '-' cannot use nullable operand '" + type_name(operand) + "'");
        }
        if (!is_unknown(operand) && !is_numeric(operand)) {
            throw_type_error(
                token_location(unary.operator_token),
                "unary '-' requires a numeric operand, got '"
                    + std::string(type_name(operand)) + "'");
        }
        return operand;
    }
    case ExprKind::Binary:
        return check_binary(static_cast<const BinaryExpr&>(expression));
    case ExprKind::Call:
        return check_call(static_cast<const CallExpr&>(expression), expected_type);
    case ExprKind::MemberAccess: {
        const auto& member = static_cast<const MemberAccessExpr&>(expression);
        return check_member_access(member);
    }
    case ExprKind::TypeAccess:
        return check_type_access(static_cast<const TypeAccessExpr&>(expression));
    case ExprKind::Propagation:
        return check_propagation(static_cast<const PropagationExpr&>(expression));
    case ExprKind::Cast:
        return check_cast(static_cast<const CastExpr&>(expression));
    case ExprKind::Grouping:
        return check_expression(
            *static_cast<const GroupingExpr&>(expression).expression,
            std::move(expected_type));
    }
    return unknown_type;
}

Type TypeChecker::check_call(
    const CallExpr& call,
    std::optional<Type> expected_type)
{
    if (call.callee->kind == ExprKind::Identifier) {
        const auto& callee = static_cast<const IdentifierExpr&>(*call.callee);
        if (callee.name == "ok" || callee.name == "error") {
            if (!expected_type
                || expected_type->name != "Result"
                || expected_type->arguments.size() != 2) {
                throw_type_error(
                    current_location_, "cannot infer '" + callee.name
                        + "' without an expected Result<T, E> type");
            }
            return check_result_construction(
                callee.name, call, *expected_type);
        }
    }

    std::vector<Type> arguments;
    arguments.reserve(call.arguments.size());
    for (const auto& argument : call.arguments) {
        arguments.push_back(require_value(
            check_expression(*argument.value), current_location_));
    }

    if (call.callee->kind == ExprKind::Identifier) {
        const auto& callee = static_cast<const IdentifierExpr&>(*call.callee);
        if (callee.name == "print") {
            if (arguments.size() != 1) {
                throw_type_error(
                    current_location_,
                    "print expects 1 argument, got " + std::to_string(arguments.size()));
            }
            return void_type;
        }

        auto candidates = find_functions(callee.name);
        const bool has_function_candidates = !candidates.empty();
        if (has_function_candidates) {
            return resolve_overload(
                "function", callee.name, call.arguments, arguments,
                call.generic_arguments, candidates)
                .return_type;
        }

        if (const auto* type_info = find_nominal_type(callee.name)) {
            return check_construction(callee.name, call, arguments, *type_info);
        }

        if (const auto callee_type = find_value(callee.name)) {
            throw_type_error(
                current_location_,
                "value '" + callee.name + "' of type '"
                    + std::string(type_name(*callee_type)) + "' is not callable");
        }
        for (const Type& argument : arguments) {
            if (argument.kind == TypeKind::Null) {
                throw_type_error(current_location_, "null requires a nullable parameter type");
            }
        }
        return unknown_type;
    }

    if (call.callee->kind == ExprKind::TypeAccess) {
        const auto& access = static_cast<const TypeAccessExpr&>(*call.callee);
        const auto* type_info = find_nominal_type(access.type_name);
        if (!type_info || type_info->kind != NominalKind::Enum) {
            throw_type_error(
                current_location_,
                "'::' type-scoped access currently supports enum variants only");
        }
        return check_enum_variant_call(
            access.type_name, access.member, call, arguments, *type_info);
    }

    if (call.callee->kind == ExprKind::MemberAccess) {
        const auto& member = static_cast<const MemberAccessExpr&>(*call.callee);
        if (member.object->kind == ExprKind::Identifier) {
            const auto& object = static_cast<const IdentifierExpr&>(*member.object);
            if (const auto* type_info = find_nominal_type(object.name);
                type_info && type_info->kind == NominalKind::Enum) {
                throw_type_error(
                    current_location_,
                    "enum variants must use '::'; '.' is instance member access");
            }
        }
        return check_method_call(member, call, arguments);
    }

    const Type callee_type = check_expression(*call.callee);
    if (!is_unknown(callee_type)) {
        throw_type_error(
            current_location_,
            "value of type '" + std::string(type_name(callee_type)) + "' is not callable");
    }
    for (const Type& argument : arguments) {
        if (argument.kind == TypeKind::Null) {
            throw_type_error(current_location_, "null requires a nullable parameter type");
        }
    }
    return unknown_type;
}

Type TypeChecker::check_result_construction(
    const std::string& constructor_name,
    const CallExpr& call,
    const Type& expected_type)
{
    if (!call.generic_arguments.empty()) {
        throw_type_error(
            current_location_, "Result constructor '" + constructor_name
                + "' does not accept explicit generic arguments");
    }
    if (call.arguments.size() != 1) {
        throw_type_error(
            current_location_, "Result constructor '" + constructor_name
                + "' expects 1 argument, got "
                + std::to_string(call.arguments.size()));
    }
    const std::size_t payload_index = constructor_name == "ok" ? 0U : 1U;
    const Type& payload_type = expected_type.arguments[payload_index];
    const Type payload = require_value(
        check_expression(*call.arguments.front().value, payload_type),
        current_location_);
    require_assignable(payload_type, payload, current_location_);
    return Type{
        TypeKind::Unknown,
        false,
        false,
        "Result",
        expected_type.arguments,
    };
}

Type TypeChecker::check_propagation(const PropagationExpr& propagation)
{
    const SourceLocation location = token_location(propagation.question_token);
    const Type result = require_value(
        check_expression(*propagation.expression), location);
    if (result.nullable || result.name != "Result" || result.arguments.size() != 2) {
        throw_type_error(
            location, "operator '?' requires Result<T, E>, got '"
                + type_name(result) + "'");
    }
    if (!current_return_type_
        || current_return_type_->nullable
        || current_return_type_->name != "Result"
        || current_return_type_->arguments.size() != 2) {
        throw_type_error(
            location, "operator '?' is only valid inside a function returning Result<T, E>");
    }
    const Type& propagated_error = result.arguments[1];
    const Type& return_error = current_return_type_->arguments[1];
    if (!is_assignable(return_error, propagated_error)) {
        throw_type_error(
            location, "cannot propagate Result error type '"
                + type_name(propagated_error) + "' from a function returning error type '"
                + type_name(return_error) + "'");
    }
    return result.arguments[0];
}

Type TypeChecker::check_construction(
    const std::string& name,
    const CallExpr& call,
    const std::vector<Type>& arguments,
    const NominalTypeInfo& type_info)
{
    if (type_info.kind == NominalKind::Enum) {
        throw_type_error(
            current_location_, "enum '" + name
                + "' must be constructed through one of its variants");
    }
    if (type_info.kind == NominalKind::Interface) {
        throw_type_error(
            current_location_, "interface '" + name + "' cannot be constructed");
    }
    if (type_info.kind == NominalKind::Class && type_info.is_abstract) {
        throw_type_error(
            current_location_, "abstract class '" + name + "' cannot be constructed");
    }
    if (!call.generic_arguments.empty()
        && call.generic_arguments.size() != type_info.generic_parameters.size()) {
        throw_type_error(
            current_location_, "type '" + name + "' expects "
                + std::to_string(type_info.generic_parameters.size())
                + " generic arguments, got "
                + std::to_string(call.generic_arguments.size()));
    }
    std::unordered_map<std::string, Type> substitutions;
    std::unordered_set<std::string> explicit_parameters;
    for (std::size_t index = 0; index < call.generic_arguments.size(); ++index) {
        const auto& parameter = type_info.generic_parameters[index];
        substitutions.emplace(parameter.name, resolve_type(call.generic_arguments[index]));
        explicit_parameters.insert(parameter.name);
    }
    std::vector<std::string> field_order;
    std::unordered_map<std::string, const FieldInfo*> fields;
    std::function<void(const NominalTypeInfo&)> collect_fields =
        [&](const NominalTypeInfo& current) {
            if (current.base) {
                if (const auto* base = find_nominal_type(*current.base)) {
                    collect_fields(*base);
                }
            }
            for (const auto& field_name : current.field_order) {
                if (!fields.contains(field_name)) {
                    field_order.push_back(field_name);
                }
                fields[field_name] = &current.fields.at(field_name);
            }
        };
    collect_fields(type_info);

    if (arguments.size() > fields.size()) {
        throw_type_error(
            current_location_,
            "construction of '" + name + "' accepts at most "
                + std::to_string(fields.size()) + " fields, got "
                + std::to_string(arguments.size()));
    }

    std::unordered_set<std::string> supplied;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        std::string field_name;
        if (call.arguments[index].name) {
            field_name = *call.arguments[index].name;
        } else {
            if (index >= field_order.size()) {
                throw_type_error(current_location_, "too many positional fields for '" + name + "'");
            }
            field_name = field_order[index];
        }

        const auto field = fields.find(field_name);
        if (field == fields.end()) {
            throw_type_error(
                current_location_,
                "type '" + name + "' has no field named '" + field_name + "'");
        }
        if (!supplied.insert(field_name).second) {
            throw_type_error(
                current_location_, "field '" + field_name + "' is supplied more than once");
        }
        if (!can_access(field->second->visibility, field->second->owner)) {
            throw_type_error(
                current_location_, "field '" + field->second->owner + "."
                    + field_name + "' is private");
        }
        std::string failure_reason;
        const bool own_generic_field = field->second->owner == name
            && !type_info.generic_parameters.empty();
        if (own_generic_field
            && !infer_type_arguments(
                field->second->type_reference, arguments[index],
                type_info.generic_parameters, substitutions, explicit_parameters,
                failure_reason)) {
            throw_type_error(
                current_location_, "field '" + name + "." + field_name
                    + "' cannot accept '" + type_name(arguments[index])
                    + "': " + failure_reason);
        }
        const Type expected = own_generic_field
            ? substitute_type(field->second->type_reference, substitutions)
            : field->second->type;
        if (!expected.deferred && !is_assignable(expected, arguments[index])) {
            throw_type_error(
                current_location_,
                "field '" + name + "." + field_name + "' expects '"
                    + type_name(expected) + "', got '"
                    + type_name(arguments[index]) + "'");
        }
    }

    for (const auto& parameter : type_info.generic_parameters) {
        if (!substitutions.contains(parameter.name)) {
            throw_type_error(
                current_location_, "cannot infer generic parameter '" + parameter.name
                    + "' while constructing '" + name + "'");
        }
    }
    std::string constraint_failure;
    if (!validate_constraints(
            type_info.generic_parameters, substitutions, constraint_failure)) {
        throw_type_error(current_location_, constraint_failure);
    }

    for (const auto& field_name : field_order) {
        const auto* field = fields.at(field_name);
        const Type field_type = field->owner == name
                && !type_info.generic_parameters.empty()
            ? substitute_type(field->type_reference, substitutions)
            : field->type;
        const bool required = !field->has_explicit_default
            && !has_zero_value(field_type);
        if (required && !supplied.contains(field_name)) {
            throw_type_error(
                current_location_,
                "construction of '" + name + "' is missing required field '"
                    + field_name + "'");
        }
    }

    std::vector<Type> type_arguments;
    type_arguments.reserve(type_info.generic_parameters.size());
    for (const auto& parameter : type_info.generic_parameters) {
        type_arguments.push_back(substitutions.at(parameter.name));
    }
    return Type{
        TypeKind::Unknown, false, false, name, std::move(type_arguments)};
}

Type TypeChecker::check_enum_variant_call(
    const std::string& enum_name,
    const std::string& variant_name,
    const CallExpr& call,
    const std::vector<Type>& arguments,
    const NominalTypeInfo& type_info) const
{
    if (!call.generic_arguments.empty()) {
        throw_type_error(
            current_location_, "enum variant '" + enum_name + "::" + variant_name
                + "' does not accept generic arguments");
    }
    const auto variant = type_info.variants.find(variant_name);
    if (variant == type_info.variants.end()) {
        throw_type_error(
            current_location_, "enum '" + enum_name + "' has no variant named '"
                + variant_name + "'");
    }
    const std::size_t expected_count = variant->second.payload_type ? 1U : 0U;
    if (arguments.size() != expected_count) {
        throw_type_error(
            current_location_, "enum variant '" + enum_name + "::" + variant_name
                + "' expects " + std::to_string(expected_count) + " payload argument"
                + (expected_count == 1 ? "" : "s") + ", got "
                + std::to_string(arguments.size()));
    }
    if (variant->second.payload_type) {
        require_assignable(
            *variant->second.payload_type, arguments.front(), current_location_);
    }
    return Type{TypeKind::Unknown, false, false, enum_name};
}

Type TypeChecker::check_method_call(
    const MemberAccessExpr& callee,
    const CallExpr& call,
    const std::vector<Type>& arguments)
{
    const Type object = require_value(check_expression(*callee.object), current_location_);
    if (object.nullable) {
        throw_type_error(
            current_location_, "cannot call a method through nullable type '"
                + type_name(object) + "'");
    }
    if (object.deferred && object.name.empty()) {
        return unknown_type;
    }
    if (!is_unknown(object) || object.name.empty()) {
        throw_type_error(
            current_location_, "type '" + type_name(object) + "' has no methods");
    }

    const auto* type_info = find_nominal_type(object.name);
    if (!type_info) {
        throw_type_error(
            current_location_, "cannot resolve members of type '" + type_name(object) + "'");
    }
    const auto methods = find_methods(object.name, callee.member);
    if (methods.empty()) {
        if (find_field(object.name, callee.member)) {
            throw_type_error(
                current_location_, "field '" + object.name + "." + callee.member
                    + "' is not callable");
        }
        throw_type_error(
            current_location_, "type '" + object.name + "' has no method named '"
                + callee.member + "'");
    }
    std::unordered_map<std::string, Type> containing_substitutions;
    if (object.arguments.size() == type_info->generic_parameters.size()) {
        for (std::size_t index = 0; index < object.arguments.size(); ++index) {
            containing_substitutions.emplace(
                type_info->generic_parameters[index].name, object.arguments[index]);
        }
    }
    std::vector<FunctionSignature> instantiated_signatures;
    instantiated_signatures.reserve(methods.size());
    std::vector<const FunctionSignature*> candidates;
    candidates.reserve(methods.size());
    for (const auto* method : methods) {
        instantiated_signatures.push_back(method->signature);
        if (method->owner == object.name) {
            instantiated_signatures.back().containing_substitutions =
                containing_substitutions;
        }
        candidates.push_back(&instantiated_signatures.back());
    }
    const auto resolution = resolve_overload(
        "method", object.name + "." + callee.member,
        call.arguments, arguments, call.generic_arguments, candidates);
    const auto selected = std::find_if(
        candidates.begin(), candidates.end(), [&](const FunctionSignature* signature) {
            return signature == resolution.signature;
        });
    const auto method_index = static_cast<std::size_t>(selected - candidates.begin());
    const auto* method = methods[method_index];
    if (!can_access(method->visibility, method->owner)) {
        throw_type_error(
            current_location_, "method '" + method->owner + "." + callee.member
                + "' is private");
    }
    return resolution.return_type;
}

TypeChecker::OverloadResolution TypeChecker::resolve_overload(
    const std::string& callable_kind,
    const std::string& callable_name,
    const std::vector<CallArgument>& call_arguments,
    const std::vector<Type>& arguments,
    const std::vector<TypeReference>& generic_arguments,
    const std::vector<const FunctionSignature*>& candidates) const
{
    int best_score = 0;
    std::vector<OverloadResolution> best;
    std::string specific_failure;
    for (const auto* candidate : candidates) {
        Type return_type = candidate->return_type;
        std::string failure_reason;
        const auto score = overload_score(
            call_arguments, arguments, generic_arguments, *candidate,
            return_type, failure_reason);
        if (!score) {
            if (specific_failure.empty() && !failure_reason.empty()) {
                specific_failure = std::move(failure_reason);
            }
            continue;
        }
        if (best.empty() || *score < best_score) {
            best_score = *score;
            best = {OverloadResolution{candidate, return_type}};
        } else if (*score == best_score) {
            best.push_back(OverloadResolution{candidate, return_type});
        }
    }
    if (best.empty()) {
        if (!specific_failure.empty()) {
            throw_type_error(
                current_location_, "no matching overload for '" + callable_name
                    + "': " + specific_failure);
        }
        if (candidates.size() == 1) {
            const auto& signature = *candidates.front();
            const std::string prefix = "no matching overload for '" + callable_name
                + "': " + callable_kind + " '" + callable_name + "' ";
            if (arguments.size() != signature.parameters.size()) {
                throw_type_error(
                    current_location_, prefix + "expects "
                        + std::to_string(signature.parameters.size())
                        + " arguments, got " + std::to_string(arguments.size()));
            }
            std::vector<bool> supplied(signature.parameters.size(), false);
            for (std::size_t index = 0; index < arguments.size(); ++index) {
                std::size_t parameter_index = index;
                if (call_arguments[index].name) {
                    const auto& argument_name = *call_arguments[index].name;
                    const auto parameter = std::find_if(
                        signature.parameters.begin(), signature.parameters.end(),
                        [&argument_name](const FunctionParameterType& candidate) {
                            return candidate.name == argument_name;
                        });
                    if (parameter == signature.parameters.end()) {
                        throw_type_error(
                            current_location_, prefix + "has no parameter named '"
                                + argument_name + "'");
                    }
                    parameter_index = static_cast<std::size_t>(
                        parameter - signature.parameters.begin());
                }
                if (supplied[parameter_index]) {
                    throw_type_error(
                        current_location_, "no matching overload for '" + callable_name
                            + "': parameter '"
                            + signature.parameters[parameter_index].name
                            + "' is supplied more than once");
                }
                supplied[parameter_index] = true;
                const Type& expected = signature.parameters[parameter_index].type;
                if (!expected.deferred && !arguments[index].deferred
                    && !is_assignable(expected, arguments[index])) {
                    throw_type_error(
                        current_location_, "no matching overload for '" + callable_name
                            + "': argument " + std::to_string(index + 1) + " to '"
                            + callable_name + "' expects '" + type_name(expected)
                            + "', got '" + type_name(arguments[index]) + "'");
                }
            }
        }
        throw_type_error(
            current_location_, "no matching overload for '" + callable_name + "'");
    }
    if (best.size() != 1) {
        throw_type_error(
            current_location_, "ambiguous overload for '" + callable_name + "'");
    }
    return best.front();
}

std::optional<int> TypeChecker::overload_score(
    const std::vector<CallArgument>& call_arguments,
    const std::vector<Type>& arguments,
    const std::vector<TypeReference>& generic_arguments,
    const FunctionSignature& signature,
    Type& return_type,
    std::string& failure_reason) const
{
    if (arguments.size() != signature.parameters.size()) {
        return std::nullopt;
    }
    const bool is_generic = !signature.generic_parameters.empty();
    if (!generic_arguments.empty() && !is_generic) {
        failure_reason = "non-generic callable does not accept explicit generic arguments";
        return std::nullopt;
    }
    if (!generic_arguments.empty()
        && generic_arguments.size() != signature.generic_parameters.size()) {
        failure_reason = "expected "
            + std::to_string(signature.generic_parameters.size())
            + " explicit generic arguments, got "
            + std::to_string(generic_arguments.size());
        return std::nullopt;
    }

    std::unordered_map<std::string, Type> substitutions =
        signature.containing_substitutions;
    std::unordered_set<std::string> explicit_parameters;
    for (std::size_t index = 0; index < generic_arguments.size(); ++index) {
        const auto& parameter = signature.generic_parameters[index];
        substitutions.insert_or_assign(
            parameter.name, resolve_type(generic_arguments[index]));
        explicit_parameters.insert(parameter.name);
    }

    int score = is_generic ? 100 : 0;
    std::vector<bool> supplied(signature.parameters.size(), false);
    const auto reference_contains = [](const TypeReference& reference,
                                        const std::string& name,
                                        const auto& self) -> bool {
        if (reference.name == name) {
            return true;
        }
        return std::any_of(
            reference.arguments.begin(), reference.arguments.end(),
            [&](const TypeReference& argument) {
                return self(argument, name, self);
            });
    };
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        std::size_t parameter_index = index;
        if (call_arguments[index].name) {
            const auto& name = *call_arguments[index].name;
            const auto parameter = std::find_if(
                signature.parameters.begin(), signature.parameters.end(),
                [&name](const FunctionParameterType& candidate) {
                    return candidate.name == name;
                });
            if (parameter == signature.parameters.end()) {
                return std::nullopt;
            }
            parameter_index = static_cast<std::size_t>(
                parameter - signature.parameters.begin());
        }
        if (supplied[parameter_index]) {
            return std::nullopt;
        }
        supplied[parameter_index] = true;

        const auto& parameter = signature.parameters[parameter_index];
        const Type& actual = arguments[index];
        const bool has_generic_reference = std::any_of(
            signature.generic_parameters.begin(), signature.generic_parameters.end(),
            [&](const GenericParameter& generic) {
                return reference_contains(
                    parameter.type_reference, generic.name, reference_contains);
            });
        if (has_generic_reference) {
            if (!infer_type_arguments(
                    parameter.type_reference, actual, signature.generic_parameters,
                    substitutions, explicit_parameters, failure_reason)) {
                return std::nullopt;
            }
            score += 2;
            continue;
        }
        const Type expected = substitute_type(parameter.type_reference, substitutions);
        if (expected == actual && !expected.deferred && !actual.deferred) {
            continue;
        }
        if (expected.deferred || actual.deferred) {
            score += 2;
            continue;
        }
        if (is_assignable(expected, actual)) {
            ++score;
            continue;
        }
        return std::nullopt;
    }

    if (!validate_constraints(
            signature.generic_parameters, substitutions, failure_reason)) {
        return std::nullopt;
    }
    return_type = signature.return_type_reference
        ? substitute_type(*signature.return_type_reference, substitutions)
        : void_type;
    return score;
}

Type TypeChecker::check_member_access(const MemberAccessExpr& member)
{
    if (member.object->kind == ExprKind::Identifier) {
        const auto& object = static_cast<const IdentifierExpr&>(*member.object);
        if (const auto* type_info = find_nominal_type(object.name);
            type_info && type_info->kind == NominalKind::Enum) {
            throw_type_error(
                current_location_,
                "enum variants must use '::'; '.' is instance member access");
        }
    }
    const Type object = require_value(check_expression(*member.object), current_location_);
    if (object.nullable) {
        throw_type_error(
            current_location_, "cannot access a member through nullable type '"
                + type_name(object) + "'");
    }
    if (object.deferred && object.name.empty()) {
        return unknown_type;
    }
    if (!is_unknown(object) || object.name.empty()) {
        throw_type_error(
            current_location_, "cannot access member '" + member.member + "' on type '"
                + type_name(object) + "'");
    }

    const auto* type_info = find_nominal_type(object.name);
    if (!type_info) {
        throw_type_error(
            current_location_, "cannot resolve members of type '" + type_name(object) + "'");
    }
    if (const auto* field = find_field(object.name, member.member)) {
        if (!can_access(field->visibility, field->owner)) {
            throw_type_error(
                current_location_, "field '" + field->owner + "." + member.member
                    + "' is private");
        }
        if (field->owner == object.name
            && object.arguments.size() == type_info->generic_parameters.size()) {
            std::unordered_map<std::string, Type> substitutions;
            for (std::size_t index = 0; index < object.arguments.size(); ++index) {
                substitutions.emplace(
                    type_info->generic_parameters[index].name,
                    object.arguments[index]);
            }
            return substitute_type(field->type_reference, substitutions);
        }
        return field->type;
    }
    const auto methods = find_methods(object.name, member.member);
    if (!methods.empty()) {
        const auto accessible = std::find_if(
            methods.begin(), methods.end(), [&](const MethodInfo* method) {
                return can_access(method->visibility, method->owner);
            });
        if (accessible == methods.end()) {
            const auto* method = methods.front();
            throw_type_error(
                current_location_, "method '" + method->owner + "." + member.member
                    + "' is private");
        }
        return unknown_type;
    }
    throw_type_error(
        current_location_, "type '" + object.name + "' has no member named '"
            + member.member + "'");
}

Type TypeChecker::check_type_access(const TypeAccessExpr& access) const
{
    const auto* type_info = find_nominal_type(access.type_name);
    if (!type_info || type_info->kind != NominalKind::Enum) {
        throw_type_error(
            current_location_,
            "'::' type-scoped access currently supports enum variants only");
    }
    const auto variant = type_info->variants.find(access.member);
    if (variant == type_info->variants.end()) {
        throw_type_error(
            current_location_, "enum '" + access.type_name
                + "' has no variant named '" + access.member + "'");
    }
    if (variant->second.payload_type) {
        throw_type_error(
            current_location_, "enum variant '" + access.type_name + "::"
                + access.member + "' requires 1 payload argument");
    }
    return Type{TypeKind::Unknown, false, false, access.type_name};
}

Type TypeChecker::check_binary(const BinaryExpr& binary)
{
    const SourceLocation location = token_location(binary.operator_token);
    const Type left = require_value(check_expression(*binary.left), location);
    const Type right = require_value(check_expression(*binary.right), location);
    if ((left.deferred && !left.name.empty())
        || (right.deferred && !right.name.empty())) {
        const Type& generic = left.deferred && !left.name.empty() ? left : right;
        throw_type_error(
            location, "operator '" + binary.operator_token.lexeme
                + "' cannot use unconstrained generic type '" + type_name(generic) + "'");
    }

    switch (binary.operator_token.type) {
    case TokenType::Plus:
    case TokenType::Minus:
    case TokenType::Star:
    case TokenType::Slash:
        if (left.nullable || right.nullable) {
            throw_type_error(location, "arithmetic cannot use nullable operands");
        }
        if (!is_unknown(left) && !is_numeric(left)) {
            throw_type_error(
                location,
                "operator '" + binary.operator_token.lexeme
                    + "' requires numeric operands, got '"
                    + std::string(type_name(left)) + "'");
        }
        if (!is_unknown(right) && !is_numeric(right)) {
            throw_type_error(
                location,
                "operator '" + binary.operator_token.lexeme
                    + "' requires numeric operands, got '"
                    + std::string(type_name(right)) + "'");
        }
        if (!is_unknown(left) && !is_unknown(right) && left != right) {
            throw_type_error(
                location,
                "operator '" + binary.operator_token.lexeme
                    + "' does not implicitly convert '" + std::string(type_name(left))
                    + "' and '" + std::string(type_name(right)) + "'");
        }
        return is_unknown(left) || is_unknown(right) ? unknown_type : left;
    case TokenType::Less:
    case TokenType::LessEqual:
    case TokenType::Greater:
    case TokenType::GreaterEqual:
        if (left.nullable || right.nullable) {
            throw_type_error(location, "comparison cannot use nullable operands");
        }
        if (!is_unknown(left) && !is_numeric(left)) {
            throw_type_error(
                location,
                "comparison requires numeric operands, got '"
                    + std::string(type_name(left)) + "'");
        }
        if (!is_unknown(right) && !is_numeric(right)) {
            throw_type_error(
                location,
                "comparison requires numeric operands, got '"
                    + std::string(type_name(right)) + "'");
        }
        if (!is_unknown(left) && !is_unknown(right) && left != right) {
            throw_type_error(
                location,
                "comparison does not implicitly convert '" + std::string(type_name(left))
                    + "' and '" + std::string(type_name(right)) + "'");
        }
        return bool_type;
    case TokenType::Equal:
    case TokenType::NotEqual: {
        if (left.kind == TypeKind::Null || right.kind == TypeKind::Null) {
            const Type other = left.kind == TypeKind::Null ? right : left;
            if (!other.nullable && !(is_unknown(other) && other.deferred)) {
                throw_type_error(
                    location,
                    "null cannot be compared with non-null type '" + type_name(other) + "'");
            }
            return bool_type;
        }
        if (!is_assignable(Type{left.kind, true, left.deferred, left.name}, right)
            && !is_assignable(Type{right.kind, true, right.deferred, right.name}, left)) {
            throw_type_error(
                location,
                "equality operands must have the same type, got '"
                    + type_name(left) + "' and '" + type_name(right) + "'");
        }
        return bool_type;
    }
    case TokenType::And:
    case TokenType::Or:
        if (left.nullable || right.nullable) {
            throw_type_error(location, "logical operators cannot use nullable operands");
        }
        if (!is_unknown(left) && left != bool_type) {
            throw_type_error(
                location,
                "operator '" + binary.operator_token.lexeme
                    + "' requires bool operands, got '"
                    + std::string(type_name(left)) + "'");
        }
        if (!is_unknown(right) && right != bool_type) {
            throw_type_error(
                location,
                "operator '" + binary.operator_token.lexeme
                    + "' requires bool operands, got '"
                    + std::string(type_name(right)) + "'");
        }
        return bool_type;
    default:
        return unknown_type;
    }
}

Type TypeChecker::check_cast(const CastExpr& cast)
{
    const SourceLocation location = cast.target_type.location;
    const Type source = require_value(check_expression(*cast.expression), location);
    const Type target = resolve_type(cast.target_type);
    reject_standalone_null_type(target, location);

    if (source.kind == TypeKind::Null) {
        if (target.nullable) {
            return target;
        }
        throw_type_error(location, "null cannot be cast to non-null type '"
            + type_name(target) + "'");
    }
    if (source.nullable && !target.nullable) {
        throw_type_error(
            location,
            "cast from nullable type '" + type_name(source) + "' to non-null type '"
                + type_name(target) + "' does not unwrap the value");
    }
    if (has_same_base_type(source, target)) {
        return target;
    }
    if (source.nullable) {
        throw_type_error(
            location,
            "nullable type '" + type_name(source)
                + "' cannot use a conversion overload");
    }
    const bool source_is_numeric = source.kind == TypeKind::Int
        || source.kind == TypeKind::Dec;
    const bool target_is_numeric = target.kind == TypeKind::Int
        || target.kind == TypeKind::Dec;
    if (source_is_numeric && target_is_numeric) {
        return target;
    }
    if (source.deferred || target.deferred) {
        return target;
    }
    if (find_conversion(source, target)) {
        return target;
    }

    throw_type_error(
        location,
        "no conversion from '" + type_name(source) + "' to '" + type_name(target) + "'");
}

void TypeChecker::check_function(const FunctionDeclarationStmt& function)
{
    const auto enclosing_return_type = current_return_type_;
    push_generic_parameters(function.generic_parameters);
    current_return_type_ = function.return_type
        ? resolve_type(*function.return_type)
        : void_type;
    reject_standalone_null_type(
        *current_return_type_,
        function.return_type ? function.return_type->location : function.location);

    push_scope();
    for (const auto& parameter : function.parameters) {
        const Type type = resolve_type(parameter.type);
        reject_standalone_null_type(type, parameter.type.location);
        declare_value(parameter.name, type);
    }
    check_statement_list(function.body->statements);
    const bool returns_on_all_paths = !function.return_type
        || statements_guarantee_return(function.body->statements);
    pop_scope();
    pop_generic_parameters();
    current_return_type_ = enclosing_return_type;
    if (!returns_on_all_paths) {
        throw_type_error(
            function.location, "function '" + function.name
                + "' does not return a value on every reachable path");
    }
}

void TypeChecker::check_method(
    const MethodDeclaration& method,
    const Type& containing_type)
{
    const auto enclosing_return_type = current_return_type_;
    const auto enclosing_type_name = current_type_name_;
    current_type_name_ = containing_type.name;
    push_generic_parameters(method.generic_parameters);
    current_return_type_ = method.return_type ? resolve_type(*method.return_type) : void_type;
    reject_standalone_null_type(
        *current_return_type_, method.return_type ? method.return_type->location : method.location);

    push_scope();
    declare_value("self", containing_type);
    for (const auto& parameter : method.parameters) {
        const Type type = resolve_type(parameter.type);
        reject_standalone_null_type(type, parameter.type.location);
        declare_value(parameter.name, type);
    }
    if (method.body) {
        check_statement_list(method.body->statements);
    }
    const bool returns_on_all_paths = !method.return_type || !method.body
        || statements_guarantee_return(method.body->statements);
    pop_scope();
    pop_generic_parameters();
    current_return_type_ = enclosing_return_type;
    current_type_name_ = enclosing_type_name;
    if (!returns_on_all_paths) {
        throw_type_error(
            method.location, "method '" + containing_type.name + "." + method.name
                + "' does not return a value on every reachable path");
    }
}

void TypeChecker::check_conversion(
    const ConversionOverload& conversion,
    const Type& source_type)
{
    const auto enclosing_return_type = current_return_type_;
    const auto enclosing_type_name = current_type_name_;
    current_type_name_ = source_type.name;
    current_return_type_ = resolve_type(conversion.target_type);
    reject_standalone_null_type(*current_return_type_, conversion.target_type.location);

    push_scope();
    declare_value("self", source_type);
    check_statement_list(conversion.body->statements);
    const bool returns_on_all_paths =
        statements_guarantee_return(conversion.body->statements);
    pop_scope();
    current_return_type_ = enclosing_return_type;
    current_type_name_ = enclosing_type_name;
    if (!returns_on_all_paths) {
        throw_type_error(
            conversion.location, "conversion from '" + source_type.name + "' to '"
                + format_type_reference(conversion.target_type)
                + "' does not return a value on every reachable path");
    }
}

void TypeChecker::check_block(const BlockStmt& block)
{
    push_scope();
    check_statement_list(block.statements);
    pop_scope();
}

bool TypeChecker::statements_guarantee_return(
    const std::vector<std::unique_ptr<Stmt>>& statements) const
{
    return std::any_of(
        statements.begin(), statements.end(),
        [&](const std::unique_ptr<Stmt>& statement) {
            return statement_guarantees_return(*statement);
        });
}

bool TypeChecker::statement_guarantees_return(const Stmt& statement) const
{
    switch (statement.kind) {
    case StmtKind::Return:
        return true;
    case StmtKind::Block:
        return statements_guarantee_return(
            static_cast<const BlockStmt&>(statement).statements);
    case StmtKind::If: {
        const auto& conditional = static_cast<const IfStmt&>(statement);
        return conditional.else_branch
            && statements_guarantee_return(conditional.then_block->statements)
            && statement_guarantees_return(*conditional.else_branch);
    }
    case StmtKind::Handle: {
        const auto& handle = static_cast<const HandleStmt&>(statement);
        return !handle.cases.empty()
            && std::all_of(
                handle.cases.begin(), handle.cases.end(),
                [&](const HandleCase& handle_case) {
                    return statements_guarantee_return(
                        handle_case.body->statements);
                });
    }
    case StmtKind::VariableDeclaration:
    case StmtKind::Assignment:
    case StmtKind::MemberAssignment:
    case StmtKind::Expression:
    case StmtKind::FunctionDeclaration:
    case StmtKind::While:
    case StmtKind::ForIn:
    case StmtKind::Stop:
    case StmtKind::Continue:
    case StmtKind::EnumDeclaration:
    case StmtKind::StructDeclaration:
    case StmtKind::ClassDeclaration:
    case StmtKind::InterfaceDeclaration:
        return false;
    }
    return false;
}

Type TypeChecker::resolve_type(const TypeReference& reference) const
{
    if (is_generic_parameter(reference.name) && reference.arguments.empty()) {
        return Type{
            TypeKind::Unknown,
            reference.nullable,
            true,
            reference.name,
        };
    }
    if (!reference.arguments.empty()) {
        std::vector<Type> arguments;
        arguments.reserve(reference.arguments.size());
        bool deferred = false;
        for (const auto& argument : reference.arguments) {
            arguments.push_back(resolve_type(argument));
            deferred = deferred || arguments.back().deferred;
        }
        return Type{
            TypeKind::Unknown,
            reference.nullable,
            deferred,
            reference.name,
            std::move(arguments),
        };
    }
    if (reference.name == "int") {
        return Type{TypeKind::Int, reference.nullable};
    }
    if (reference.name == "dec") {
        return Type{TypeKind::Dec, reference.nullable};
    }
    if (reference.name == "string") {
        return Type{TypeKind::String, reference.nullable};
    }
    if (reference.name == "bool") {
        return Type{TypeKind::Bool, reference.nullable};
    }
    if (reference.name == "null") {
        return null_type;
    }
    return Type{TypeKind::Unknown, reference.nullable, false, reference.name};
}

Type TypeChecker::substitute_type(
    const TypeReference& reference,
    const std::unordered_map<std::string, Type>& substitutions) const
{
    if (reference.arguments.empty()) {
        if (const auto substitution = substitutions.find(reference.name);
            substitution != substitutions.end()) {
            Type result = substitution->second;
            if (reference.nullable) {
                result.nullable = true;
            }
            return result;
        }
        return resolve_type(reference);
    }

    std::vector<Type> arguments;
    arguments.reserve(reference.arguments.size());
    bool deferred = false;
    for (const auto& argument : reference.arguments) {
        arguments.push_back(substitute_type(argument, substitutions));
        deferred = deferred || arguments.back().deferred;
    }
    return Type{
        TypeKind::Unknown,
        reference.nullable,
        deferred,
        reference.name,
        std::move(arguments),
    };
}

bool TypeChecker::infer_type_arguments(
    const TypeReference& pattern,
    const Type& actual,
    const std::vector<GenericParameter>& generic_parameters,
    std::unordered_map<std::string, Type>& substitutions,
    const std::unordered_set<std::string>& explicit_parameters,
    std::string& failure_reason) const
{
    const auto generic = std::find_if(
        generic_parameters.begin(), generic_parameters.end(),
        [&](const GenericParameter& parameter) {
            return parameter.name == pattern.name && pattern.arguments.empty();
        });
    if (generic != generic_parameters.end()) {
        if (actual.kind == TypeKind::Null && !substitutions.contains(generic->name)) {
            failure_reason = "cannot infer generic parameter '" + generic->name
                + "' from null";
            return false;
        }
        auto substitution = substitutions.find(generic->name);
        if (substitution == substitutions.end()) {
            Type inferred = actual;
            if (pattern.nullable) {
                inferred.nullable = false;
            }
            substitutions.emplace(generic->name, std::move(inferred));
            return true;
        }

        Type expected = substitution->second;
        if (pattern.nullable) {
            expected.nullable = true;
        }
        if (is_assignable(expected, actual)) {
            return true;
        }
        if (!explicit_parameters.contains(generic->name)
            && is_assignable(actual, substitution->second)) {
            substitution->second = actual;
            return true;
        }
        failure_reason = "conflicting inference for generic parameter '"
            + generic->name + "': '" + type_name(substitution->second)
            + "' and '" + type_name(actual) + "'";
        return false;
    }

    if (!pattern.arguments.empty()) {
        if (!is_unknown(actual) || actual.name != pattern.name
            || actual.arguments.size() != pattern.arguments.size()
            || (actual.nullable && !pattern.nullable)) {
            failure_reason = "type '" + type_name(actual)
                + "' does not match generic structure '"
                + format_type_reference(pattern) + "'";
            return false;
        }
        for (std::size_t index = 0; index < pattern.arguments.size(); ++index) {
            if (!infer_type_arguments(
                    pattern.arguments[index], actual.arguments[index], generic_parameters,
                    substitutions, explicit_parameters, failure_reason)) {
                return false;
            }
        }
        return true;
    }

    const Type expected = substitute_type(pattern, substitutions);
    if (!is_assignable(expected, actual)) {
        failure_reason = "cannot use argument of type '" + type_name(actual)
            + "' where '" + type_name(expected) + "' is required";
        return false;
    }
    return true;
}

bool TypeChecker::validate_constraints(
    const std::vector<GenericParameter>& generic_parameters,
    const std::unordered_map<std::string, Type>& substitutions,
    std::string& failure_reason) const
{
    for (const auto& parameter : generic_parameters) {
        const auto substitution = substitutions.find(parameter.name);
        if (substitution == substitutions.end()) {
            failure_reason = "could not infer generic parameter '" + parameter.name + "'";
            return false;
        }
        for (const auto& constraint : parameter.constraints) {
            const Type constraint_type = substitute_type(constraint, substitutions);
            const auto* constraint_info = find_nominal_type(constraint_type.name);
            if (!constraint_info || constraint_info->kind != NominalKind::Interface) {
                failure_reason = "generic constraint '" + type_name(constraint_type)
                    + "' must name an existing interface";
                return false;
            }
            const Type& concrete = substitution->second;
            if (!concrete.deferred
                && (concrete.name.empty()
                    || !is_subtype(concrete.name, constraint_type.name))) {
                failure_reason = "type '" + type_name(concrete)
                    + "' does not satisfy constraint '" + type_name(constraint_type)
                    + "' for generic parameter '" + parameter.name + "'";
                return false;
            }
        }
    }
    return true;
}

bool TypeChecker::is_generic_parameter(const std::string& name) const
{
    for (auto scope = generic_parameter_scopes_.rbegin();
        scope != generic_parameter_scopes_.rend(); ++scope) {
        if (scope->contains(name)) {
            return true;
        }
    }
    return false;
}

bool TypeChecker::contains_generic_parameter(const TypeReference& reference) const
{
    if (is_generic_parameter(reference.name)) {
        return true;
    }
    for (const auto& argument : reference.arguments) {
        if (contains_generic_parameter(argument)) {
            return true;
        }
    }
    return false;
}

Type TypeChecker::require_value(Type type, SourceLocation location) const
{
    if (type.kind == TypeKind::Void) {
        throw_type_error(location, "expression does not produce a value");
    }
    return type;
}

void TypeChecker::require_assignable(
    Type expected,
    Type actual,
    SourceLocation location) const
{
    if (actual.kind == TypeKind::Null && !expected.nullable) {
        throw_type_error(location, "null requires a nullable type");
    }
    if (!is_assignable(expected, actual)) {
        throw_type_error(
            location,
            "cannot assign value of type '" + std::string(type_name(actual))
                + "' to type '" + std::string(type_name(expected)) + "'");
    }
}

bool TypeChecker::is_assignable(const Type& expected, const Type& actual) const
{
    if (is_directly_assignable(expected, actual)) {
        return true;
    }
    if (actual.nullable && !expected.nullable) {
        return false;
    }
    if (!is_unknown(expected) || !is_unknown(actual)
        || expected.name.empty() || actual.name.empty()) {
        return false;
    }
    if (expected.name == actual.name) {
        return false;
    }
    return is_subtype(actual.name, expected.name);
}

bool TypeChecker::is_subtype(
    const std::string& actual,
    const std::string& expected) const
{
    if (actual == expected) {
        return true;
    }
    const auto* type_info = find_nominal_type(actual);
    if (!type_info) {
        return false;
    }
    for (const auto& interface_name : type_info->interfaces) {
        if (interface_name == expected) {
            return true;
        }
    }
    return type_info->base && is_subtype(*type_info->base, expected);
}

bool TypeChecker::signatures_match(
    const FunctionSignature& left,
    const FunctionSignature& right) const
{
    return parameter_types_match(left, right)
        && left.return_type == right.return_type;
}

bool TypeChecker::parameter_types_match(
    const FunctionSignature& left,
    const FunctionSignature& right) const
{
    if (left.parameters.size() != right.parameters.size()
        || left.generic_parameters.size() != right.generic_parameters.size()) {
        return false;
    }
    const auto canonical_type = [](const TypeReference& reference,
                                   const FunctionSignature& signature,
                                   const auto& self) -> std::string {
        std::string result = reference.name;
        const auto generic = std::find_if(
            signature.generic_parameters.begin(), signature.generic_parameters.end(),
            [&](const GenericParameter& parameter) {
                return parameter.name == reference.name;
            });
        if (generic != signature.generic_parameters.end()) {
            result = "$" + std::to_string(
                generic - signature.generic_parameters.begin());
        }
        if (!reference.arguments.empty()) {
            result += '<';
            for (std::size_t index = 0; index < reference.arguments.size(); ++index) {
                if (index != 0) {
                    result += ',';
                }
                result += self(reference.arguments[index], signature, self);
            }
            result += '>';
        }
        if (reference.nullable) {
            result += '?';
        }
        return result;
    };
    for (std::size_t index = 0; index < left.parameters.size(); ++index) {
        if (canonical_type(
                left.parameters[index].type_reference, left, canonical_type)
            != canonical_type(
                right.parameters[index].type_reference, right, canonical_type)) {
            return false;
        }
    }
    return true;
}

void TypeChecker::require_condition(Type type, SourceLocation location) const
{
    type = require_value(type, location);
    if (type.nullable) {
        throw_type_error(
            location,
            "condition must have type 'bool', got nullable type '" + type_name(type) + "'");
    }
    if (!is_unknown(type) && type != bool_type) {
        throw_type_error(
            location,
            "condition must have type 'bool', got '" + std::string(type_name(type)) + "'");
    }
}

void TypeChecker::push_scope()
{
    scopes_.emplace_back();
}

void TypeChecker::pop_scope()
{
    scopes_.pop_back();
}

void TypeChecker::push_generic_parameters(
    const std::vector<GenericParameter>& parameters)
{
    auto& scope = generic_parameter_scopes_.emplace_back();
    for (const auto& parameter : parameters) {
        scope.insert(parameter.name);
    }
}

void TypeChecker::pop_generic_parameters()
{
    generic_parameter_scopes_.pop_back();
}

void TypeChecker::declare_value(const std::string& name, Type type)
{
    scopes_.back().values.emplace(name, type);
}

void TypeChecker::declare_conversion(
    const std::string& source_name,
    const Type& target_type)
{
    scopes_.back().conversions[source_name].insert(type_name(target_type));
}

std::optional<Type> TypeChecker::find_value(const std::string& name) const
{
    for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope) {
        if (const auto value = scope->values.find(name); value != scope->values.end()) {
            return value->second;
        }
        if (scope->functions.contains(name)) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

std::vector<const TypeChecker::FunctionSignature*> TypeChecker::find_functions(
    const std::string& name) const
{
    for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope) {
        if (scope->values.contains(name)) {
            return {};
        }
        if (const auto function = scope->functions.find(name);
            function != scope->functions.end()) {
            std::vector<const FunctionSignature*> result;
            result.reserve(function->second.size());
            for (const auto& signature : function->second) {
                result.push_back(&signature);
            }
            return result;
        }
    }
    return {};
}

const TypeChecker::NominalTypeInfo* TypeChecker::find_nominal_type(
    const std::string& name) const
{
    for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope) {
        if (const auto type = scope->nominal_types.find(name);
            type != scope->nominal_types.end()) {
            return &type->second;
        }
        if (scope->values.contains(name)) {
            return nullptr;
        }
    }
    return nullptr;
}

const TypeChecker::FieldInfo* TypeChecker::find_field(
    const std::string& type_name_value,
    const std::string& field_name) const
{
    const auto* type_info = find_nominal_type(type_name_value);
    if (!type_info) {
        return nullptr;
    }
    if (const auto field = type_info->fields.find(field_name);
        field != type_info->fields.end()) {
        return &field->second;
    }
    return type_info->base ? find_field(*type_info->base, field_name) : nullptr;
}

std::vector<const TypeChecker::MethodInfo*> TypeChecker::find_methods(
    const std::string& type_name_value,
    const std::string& method_name) const
{
    const auto* type_info = find_nominal_type(type_name_value);
    if (!type_info) {
        return {};
    }
    std::vector<const MethodInfo*> result;
    if (const auto method = type_info->methods.find(method_name);
        method != type_info->methods.end()) {
        for (const auto& overload : method->second) {
            result.push_back(&overload);
        }
    }
    if (type_info->base) {
        for (const auto* inherited : find_methods(*type_info->base, method_name)) {
            const bool replaced = std::any_of(
                result.begin(), result.end(), [&](const MethodInfo* candidate) {
                    return parameter_types_match(
                        candidate->signature, inherited->signature);
                });
            if (!replaced) {
                result.push_back(inherited);
            }
        }
    }
    return result;
}

bool TypeChecker::can_access(
    Visibility visibility,
    const std::string& owner) const
{
    return visibility == Visibility::Public
        || (current_type_name_ && *current_type_name_ == owner);
}

bool TypeChecker::find_conversion(
    const Type& source_type,
    const Type& target_type) const
{
    if (source_type.name.empty()) {
        return false;
    }
    for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope) {
        const auto source = scope->conversions.find(source_type.name);
        if (source != scope->conversions.end()) {
            return source->second.contains(type_name(target_type));
        }
        if (scope->values.contains(source_type.name)) {
            return false;
        }
    }
    return false;
}

} // namespace toro
