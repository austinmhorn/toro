#include "toro/TypeChecker.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace toro {
namespace {

constexpr Type int_type{TypeKind::Int};
constexpr Type dec_type{TypeKind::Dec};
constexpr Type string_type{TypeKind::String};
constexpr Type bool_type{TypeKind::Bool};
constexpr Type null_type{TypeKind::Null};
constexpr Type void_type{TypeKind::Void};
constexpr Type unknown_type{TypeKind::Unknown};

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

bool is_assignable(Type expected, Type actual)
{
    if (actual.kind == TypeKind::Null) {
        return expected.kind == TypeKind::Null;
    }
    return is_unknown(expected) || is_unknown(actual) || expected == actual;
}

} // namespace

void TypeChecker::check(const Program& program)
{
    scopes_.clear();
    current_return_type_.reset();
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
            continue;
        }

        switch (statement->kind) {
        case StmtKind::StructDeclaration:
            declare_value(
                static_cast<const StructDeclarationStmt&>(*statement).name, unknown_type);
            break;
        case StmtKind::ClassDeclaration:
            declare_value(
                static_cast<const ClassDeclarationStmt&>(*statement).name, unknown_type);
            break;
        case StmtKind::InterfaceDeclaration:
            declare_value(
                static_cast<const InterfaceDeclarationStmt&>(*statement).name, unknown_type);
            break;
        case StmtKind::EnumDeclaration:
            declare_value(
                static_cast<const EnumDeclarationStmt&>(*statement).name, unknown_type);
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
        static_cast<void>(check_expression(*assignment.target));
        const Type value = require_value(
            check_expression(*assignment.value), assignment.location);
        if (value.kind == TypeKind::Null) {
            throw_type_error(assignment.location, "null requires a nullable type");
        }
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
        return;
    }
    case StmtKind::ClassDeclaration: {
        const auto& declaration = static_cast<const ClassDeclarationStmt&>(statement);
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
            } else {
                check_method(static_cast<const MethodDeclaration&>(*member));
            }
        }
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
        static_cast<void>(require_value(check_expression(*member.object), current_location_));
        return unknown_type;
    }
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
            if (arguments.size() != signature->parameters.size()) {
                throw_type_error(
                    current_location_,
                    "function '" + callee.name + "' expects "
                        + std::to_string(signature->parameters.size()) + " arguments, got "
                        + std::to_string(arguments.size()));
            }
            std::vector<bool> supplied(signature->parameters.size(), false);
            for (std::size_t index = 0; index < arguments.size(); ++index) {
                std::size_t parameter_index = index;
                if (call.arguments[index].name) {
                    const auto& name = *call.arguments[index].name;
                    const auto parameter = std::find_if(
                        signature->parameters.begin(),
                        signature->parameters.end(),
                        [&name](const FunctionParameterType& candidate) {
                            return candidate.name == name;
                        });
                    if (parameter == signature->parameters.end()) {
                        throw_type_error(
                            current_location_,
                            "function '" + callee.name + "' has no parameter named '"
                                + name + "'");
                    }
                    parameter_index = static_cast<std::size_t>(
                        parameter - signature->parameters.begin());
                }
                if (supplied[parameter_index]) {
                    throw_type_error(
                        current_location_,
                        "parameter '" + signature->parameters[parameter_index].name
                            + "' is supplied more than once");
                }
                supplied[parameter_index] = true;

                const Type expected = signature->parameters[parameter_index].type;
                if (!is_assignable(expected, arguments[index])) {
                    throw_type_error(
                        current_location_,
                        "argument " + std::to_string(index + 1) + " to '" + callee.name
                            + "' expects '"
                            + std::string(type_name(expected))
                            + "', got '" + std::string(type_name(arguments[index])) + "'");
                }
            }
            return signature->return_type;
        }

        if (const auto callee_type = find_value(callee.name);
            callee_type && !is_unknown(*callee_type)) {
            throw_type_error(
                current_location_,
                "value '" + callee.name + "' of type '"
                    + std::string(type_name(*callee_type)) + "' is not callable");
        }
        for (const Type argument : arguments) {
            if (argument.kind == TypeKind::Null) {
                throw_type_error(current_location_, "null requires a nullable parameter type");
            }
        }
        return unknown_type;
    }

    const Type callee_type = check_expression(*call.callee);
    if (!is_unknown(callee_type)) {
        throw_type_error(
            current_location_,
            "value of type '" + std::string(type_name(callee_type)) + "' is not callable");
    }
    for (const Type argument : arguments) {
        if (argument.kind == TypeKind::Null) {
            throw_type_error(current_location_, "null requires a nullable parameter type");
        }
    }
    return unknown_type;
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
    case TokenType::NotEqual:
        if (!is_unknown(left) && !is_unknown(right) && left != right) {
            if (left.kind == TypeKind::Null || right.kind == TypeKind::Null) {
                throw_type_error(
                    location,
                    "null cannot be compared with non-null type '"
                        + std::string(type_name(
                            left.kind == TypeKind::Null ? right : left)) + "'");
            }
            throw_type_error(
                location,
                "equality operands must have the same type, got '"
                    + std::string(type_name(left)) + "' and '"
                    + std::string(type_name(right)) + "'");
        }
        return bool_type;
    case TokenType::And:
    case TokenType::Or:
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

void TypeChecker::check_function(const FunctionDeclarationStmt& function)
{
    const auto enclosing_return_type = current_return_type_;
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
    current_return_type_ = enclosing_return_type;
}

void TypeChecker::check_method(const MethodDeclaration& method)
{
    const auto enclosing_return_type = current_return_type_;
    current_return_type_ = method.return_type ? resolve_type(*method.return_type) : void_type;
    reject_standalone_null_type(
        *current_return_type_, method.return_type ? method.return_type->location : method.location);

    push_scope();
    declare_value("self", unknown_type);
    for (const auto& parameter : method.parameters) {
        const Type type = resolve_type(parameter.type);
        reject_standalone_null_type(type, parameter.type.location);
        declare_value(parameter.name, type);
    }
    if (method.body) {
        check_statement_list(method.body->statements);
    }
    pop_scope();
    current_return_type_ = enclosing_return_type;
}

void TypeChecker::check_block(const BlockStmt& block)
{
    push_scope();
    check_statement_list(block.statements);
    pop_scope();
}

Type TypeChecker::resolve_type(const TypeReference& reference) const
{
    if (!reference.arguments.empty()) {
        return unknown_type;
    }
    if (reference.name == "int") {
        return int_type;
    }
    if (reference.name == "dec") {
        return dec_type;
    }
    if (reference.name == "string") {
        return string_type;
    }
    if (reference.name == "bool") {
        return bool_type;
    }
    if (reference.name == "null") {
        return null_type;
    }
    return unknown_type;
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
    if (actual.kind == TypeKind::Null && expected.kind != TypeKind::Null) {
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

void TypeChecker::declare_value(const std::string& name, Type type)
{
    scopes_.back().values.emplace(name, type);
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

} // namespace toro
