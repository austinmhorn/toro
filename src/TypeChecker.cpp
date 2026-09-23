#include "toro/TypeChecker.hpp"

#include <algorithm>
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

bool is_assignable(const Type& expected, const Type& actual)
{
    if (actual.kind == TypeKind::Null) {
        return expected.nullable;
    }
    if (actual.nullable && !expected.nullable) {
        return false;
    }
    if (is_unknown(expected) || is_unknown(actual)) {
        if (is_unknown(expected) && is_unknown(actual)) {
            return expected.name.empty() || actual.name.empty() || expected.name == actual.name;
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
            return left.name.empty() || right.name.empty() || left.name == right.name;
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
    check_statement_list(program.statements);
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
            };
            signature.parameters.reserve(function.parameters.size());
            for (const auto& parameter : function.parameters) {
                signature.parameters.push_back(FunctionParameterType{
                    parameter.name,
                    resolve_type(parameter.type),
                });
            }
            scopes_.back().functions.emplace(function.name, std::move(signature));
            pop_generic_parameters();
            continue;
        }

        switch (statement->kind) {
        case StmtKind::StructDeclaration: {
            const auto& declaration = static_cast<const StructDeclarationStmt&>(*statement);
            declare_value(declaration.name,
                Type{TypeKind::Unknown, false, false, declaration.name});
            push_generic_parameters(declaration.generic_parameters);
            NominalTypeInfo type_info;
            for (const auto& field : declaration.fields) {
                const Type field_type = resolve_type(field.type);
                type_info.field_order.push_back(field.name);
                type_info.fields.emplace(field.name, FieldInfo{
                    field_type,
                    Visibility::Public,
                    field.default_value == nullptr && !has_zero_value(field_type),
                });
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
            for (const auto& member : declaration.members) {
                if (member->kind == ClassMemberKind::Field) {
                    const auto& field = static_cast<const ClassField&>(*member);
                    const Type field_type = resolve_type(field.type);
                    type_info.field_order.push_back(field.name);
                    type_info.fields.emplace(field.name, FieldInfo{
                        field_type,
                        field.visibility,
                        field.default_value == nullptr && !has_zero_value(field_type),
                    });
                } else if (member->kind == ClassMemberKind::Method) {
                    const auto& method = static_cast<const MethodDeclaration&>(*member);
                    push_generic_parameters(method.generic_parameters);
                    FunctionSignature signature{
                        {},
                        method.return_type ? resolve_type(*method.return_type) : void_type,
                    };
                    signature.parameters.reserve(method.parameters.size());
                    for (const auto& parameter : method.parameters) {
                        signature.parameters.push_back(FunctionParameterType{
                            parameter.name,
                            resolve_type(parameter.type),
                        });
                    }
                    pop_generic_parameters();
                    type_info.methods.emplace(method.name, MethodInfo{
                        std::move(signature),
                        method.visibility,
                    });
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
        case StmtKind::InterfaceDeclaration:
            declare_value(static_cast<const InterfaceDeclarationStmt&>(*statement).name,
                Type{TypeKind::Unknown, false, false,
                    static_cast<const InterfaceDeclarationStmt&>(*statement).name});
            break;
        case StmtKind::EnumDeclaration:
            declare_value(static_cast<const EnumDeclarationStmt&>(*statement).name,
                Type{TypeKind::Unknown, false, false,
                    static_cast<const EnumDeclarationStmt&>(*statement).name});
            break;
        default:
            break;
        }
    }
}

void TypeChecker::check_statement(const Stmt& statement)
{
    current_location_ = statement.location;
    switch (statement.kind) {
    case StmtKind::VariableDeclaration: {
        const auto& declaration = static_cast<const VariableDeclarationStmt&>(statement);
        const Type initializer = require_value(
            check_expression(*declaration.initializer), declaration.location);
        if (declaration.explicit_type) {
            const Type declared = resolve_type(*declaration.explicit_type);
            reject_standalone_null_type(declared, declaration.explicit_type->location);
            require_assignable(declared, initializer, declaration.location);
            declare_value(declaration.name, declared);
        } else {
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
            check_expression(*assignment.value), assignment.location);
        if (expected) {
            require_assignable(*expected, actual, assignment.location);
        }
        return;
    }
    case StmtKind::MemberAssignment: {
        const auto& assignment = static_cast<const MemberAssignmentStmt&>(statement);
        const Type expected = check_member_access(*assignment.target);
        const Type value = require_value(
            check_expression(*assignment.value), assignment.location);
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
            check_expression(*return_statement.value), statement.location);
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
        static_cast<void>(require_value(
            check_expression(*handle.expression), statement.location));
        for (const auto& handle_case : handle.cases) {
            push_scope();
            if (handle_case.binding_name) {
                declare_value(*handle_case.binding_name, unknown_type);
            }
            check_statement_list(handle_case.body->statements);
            pop_scope();
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
                    check_expression(*field.default_value), field.location);
                require_assignable(declared, actual, field.location);
            }
        }
        const Type source_type{TypeKind::Unknown, false, false, declaration.name};
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
                if (field.default_value) {
                    const Type actual = require_value(
                        check_expression(*field.default_value), field.location);
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

Type TypeChecker::check_expression(const Expr& expression)
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
        if (find_function(identifier.name) || identifier.name == "print") {
            return unknown_type;
        }
        return unknown_type;
    }
    case ExprKind::Unary: {
        const auto& unary = static_cast<const UnaryExpr&>(expression);
        const Type operand = require_value(
            check_expression(*unary.operand), token_location(unary.operator_token));
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
        return check_call(static_cast<const CallExpr&>(expression));
    case ExprKind::MemberAccess: {
        const auto& member = static_cast<const MemberAccessExpr&>(expression);
        return check_member_access(member);
    }
    case ExprKind::Cast:
        return check_cast(static_cast<const CastExpr&>(expression));
    case ExprKind::Grouping:
        return check_expression(*static_cast<const GroupingExpr&>(expression).expression);
    }
    return unknown_type;
}

Type TypeChecker::check_call(const CallExpr& call)
{
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

        if (const auto* signature = find_function(callee.name)) {
            check_arguments(
                "function", callee.name, call.arguments, arguments, *signature);
            return signature->return_type;
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

    if (call.callee->kind == ExprKind::MemberAccess) {
        return check_method_call(
            static_cast<const MemberAccessExpr&>(*call.callee), call, arguments);
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

Type TypeChecker::check_construction(
    const std::string& name,
    const CallExpr& call,
    const std::vector<Type>& arguments,
    const NominalTypeInfo& type_info)
{
    if (arguments.size() > type_info.fields.size()) {
        throw_type_error(
            current_location_,
            "construction of '" + name + "' accepts at most "
                + std::to_string(type_info.fields.size()) + " fields, got "
                + std::to_string(arguments.size()));
    }

    std::unordered_set<std::string> supplied;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        std::string field_name;
        if (call.arguments[index].name) {
            field_name = *call.arguments[index].name;
        } else {
            if (index >= type_info.field_order.size()) {
                throw_type_error(current_location_, "too many positional fields for '" + name + "'");
            }
            field_name = type_info.field_order[index];
        }

        const auto field = type_info.fields.find(field_name);
        if (field == type_info.fields.end()) {
            throw_type_error(
                current_location_,
                "type '" + name + "' has no field named '" + field_name + "'");
        }
        if (!supplied.insert(field_name).second) {
            throw_type_error(
                current_location_, "field '" + field_name + "' is supplied more than once");
        }
        if (!can_access(field->second.visibility, name)) {
            throw_type_error(
                current_location_, "field '" + name + "." + field_name + "' is private");
        }
        if (!is_assignable(field->second.type, arguments[index])) {
            throw_type_error(
                current_location_,
                "field '" + name + "." + field_name + "' expects '"
                    + type_name(field->second.type) + "', got '"
                    + type_name(arguments[index]) + "'");
        }
    }

    for (const auto& field_name : type_info.field_order) {
        const auto& field = type_info.fields.at(field_name);
        if (field.required && !supplied.contains(field_name)) {
            throw_type_error(
                current_location_,
                "construction of '" + name + "' is missing required field '"
                    + field_name + "'");
        }
    }

    return Type{TypeKind::Unknown, false, false, name};
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
    const auto method = type_info->methods.find(callee.member);
    if (method == type_info->methods.end()) {
        if (type_info->fields.contains(callee.member)) {
            throw_type_error(
                current_location_, "field '" + object.name + "." + callee.member
                    + "' is not callable");
        }
        throw_type_error(
            current_location_, "type '" + object.name + "' has no method named '"
                + callee.member + "'");
    }
    if (!can_access(method->second.visibility, object.name)) {
        throw_type_error(
            current_location_, "method '" + object.name + "." + callee.member
                + "' is private");
    }
    check_arguments(
        "method", object.name + "." + callee.member,
        call.arguments, arguments, method->second.signature);
    return method->second.signature.return_type;
}

void TypeChecker::check_arguments(
    const std::string& callable_kind,
    const std::string& callable_name,
    const std::vector<CallArgument>& call_arguments,
    const std::vector<Type>& arguments,
    const FunctionSignature& signature) const
{
    if (arguments.size() != signature.parameters.size()) {
        throw_type_error(
            current_location_, callable_kind + " '" + callable_name + "' expects "
                + std::to_string(signature.parameters.size()) + " arguments, got "
                + std::to_string(arguments.size()));
    }

    std::vector<bool> supplied(signature.parameters.size(), false);
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
                throw_type_error(
                    current_location_, callable_kind + " '" + callable_name
                        + "' has no parameter named '" + name + "'");
            }
            parameter_index = static_cast<std::size_t>(
                parameter - signature.parameters.begin());
        }
        if (supplied[parameter_index]) {
            throw_type_error(
                current_location_, "parameter '" + signature.parameters[parameter_index].name
                    + "' is supplied more than once");
        }
        supplied[parameter_index] = true;

        const Type expected = signature.parameters[parameter_index].type;
        if (!is_assignable(expected, arguments[index])) {
            throw_type_error(
                current_location_, "argument " + std::to_string(index + 1) + " to '"
                    + callable_name + "' expects '" + type_name(expected) + "', got '"
                    + type_name(arguments[index]) + "'");
        }
    }
}

Type TypeChecker::check_member_access(const MemberAccessExpr& member)
{
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
    if (const auto field = type_info->fields.find(member.member);
        field != type_info->fields.end()) {
        if (!can_access(field->second.visibility, object.name)) {
            throw_type_error(
                current_location_, "field '" + object.name + "." + member.member
                    + "' is private");
        }
        return field->second.type;
    }
    if (const auto method = type_info->methods.find(member.member);
        method != type_info->methods.end()) {
        if (!can_access(method->second.visibility, object.name)) {
            throw_type_error(
                current_location_, "method '" + object.name + "." + member.member
                    + "' is private");
        }
        return unknown_type;
    }
    throw_type_error(
        current_location_, "type '" + object.name + "' has no member named '"
            + member.member + "'");
}

Type TypeChecker::check_binary(const BinaryExpr& binary)
{
    const SourceLocation location = token_location(binary.operator_token);
    const Type left = require_value(check_expression(*binary.left), location);
    const Type right = require_value(check_expression(*binary.right), location);

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
    pop_scope();
    pop_generic_parameters();
    current_return_type_ = enclosing_return_type;
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
    pop_scope();
    pop_generic_parameters();
    current_return_type_ = enclosing_return_type;
    current_type_name_ = enclosing_type_name;
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
    pop_scope();
    current_return_type_ = enclosing_return_type;
    current_type_name_ = enclosing_type_name;
}

void TypeChecker::check_block(const BlockStmt& block)
{
    push_scope();
    check_statement_list(block.statements);
    pop_scope();
}

Type TypeChecker::resolve_type(const TypeReference& reference) const
{
    if (contains_generic_parameter(reference)) {
        return Type{TypeKind::Unknown, reference.nullable, true};
    }
    if (!reference.arguments.empty()) {
        TypeReference base = reference;
        base.nullable = false;
        return Type{
            TypeKind::Unknown,
            reference.nullable,
            false,
            format_type_reference(base),
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

const TypeChecker::FunctionSignature* TypeChecker::find_function(
    const std::string& name) const
{
    for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope) {
        if (scope->values.contains(name)) {
            return nullptr;
        }
        if (const auto function = scope->functions.find(name);
            function != scope->functions.end()) {
            return &function->second;
        }
    }
    return nullptr;
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
