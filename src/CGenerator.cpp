#include "toro/CGenerator.hpp"

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace toro {
namespace {

enum class CValueType { Int, Dec, Bool, String, Void };

struct ValueInfo {
    CValueType type;
    std::string c_name;
};

struct FunctionInfo {
    const FunctionDeclarationStmt* declaration;
    CValueType return_type;
    std::vector<CValueType> parameter_types;
};

struct GeneratedExpression {
    std::string code;
    CValueType type;
};

[[noreturn]] void throw_backend_error(
    SourceLocation location,
    const std::string& message)
{
    throw std::runtime_error(
        "line " + std::to_string(location.line) + ", column "
        + std::to_string(location.column) + ": backend error: " + message);
}

std::string indent(std::size_t depth)
{
    return std::string(depth * 4, ' ');
}

std::string function_name(std::string_view name)
{
    return "toro_fn_" + std::string(name);
}

std::string parameter_name(std::string_view name)
{
    return "toro_arg_" + std::string(name);
}

std::string variable_name(std::string_view name)
{
    return "toro_var_" + std::string(name);
}

std::string c_type_name(CValueType type)
{
    switch (type) {
    case CValueType::Int: return "int64_t";
    case CValueType::Dec: return "double";
    case CValueType::Bool: return "bool";
    case CValueType::String: return "const char*";
    case CValueType::Void: return "void";
    }
    return "void";
}

std::string escape_c_string(std::string_view value)
{
    std::string result;
    result.reserve(value.size() + 2);
    result += '"';
    for (const char character : value) {
        switch (character) {
        case '\\': result += "\\\\"; break;
        case '"': result += "\\\""; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default: result += character; break;
        }
    }
    result += '"';
    return result;
}

class Generator {
public:
    std::string generate(const Program& program)
    {
        collect_functions(program);

        std::string output =
            "#include <stdbool.h>\n"
            "#include <stdint.h>\n"
            "#include <stdio.h>\n"
            "#include <string.h>\n\n";

        for (const auto& statement : program.statements) {
            if (statement->kind != StmtKind::FunctionDeclaration) {
                throw_backend_error(
                    statement->location,
                    "top-level statements are not supported by the C backend");
            }
            output += function_declaration(
                static_cast<const FunctionDeclarationStmt&>(*statement));
            output += ";\n";
        }
        if (!program.statements.empty()) {
            output += '\n';
        }

        for (const auto& statement : program.statements) {
            output += emit_function(
                static_cast<const FunctionDeclarationStmt&>(*statement));
            output += '\n';
        }

        const auto main_function = functions_.find("main");
        if (main_function != functions_.end()) {
            const auto& main = main_function->second;
            if (!main.parameter_types.empty()) {
                throw_backend_error(
                    main.declaration->location,
                    "toro entry function 'main' cannot have parameters in the C backend");
            }
            output += "int main(void)\n{\n";
            if (main.return_type == CValueType::Void) {
                output += "    " + function_name("main") + "();\n";
                output += "    return 0;\n";
            } else if (main.return_type == CValueType::Int) {
                output += "    return (int)" + function_name("main") + "();\n";
            } else {
                throw_backend_error(
                    main.declaration->location,
                    "toro entry function 'main' must return int or have no return type");
            }
            output += "}\n";
        }

        return output;
    }

private:
    void collect_functions(const Program& program)
    {
        for (const auto& statement : program.statements) {
            if (statement->kind != StmtKind::FunctionDeclaration) {
                continue;
            }
            const auto& function =
                static_cast<const FunctionDeclarationStmt&>(*statement);
            if (!function.generic_parameters.empty()) {
                throw_backend_error(
                    function.location,
                    "generic functions are not supported by the C backend");
            }
            FunctionInfo info{&function, CValueType::Void, {}};
            if (function.return_type) {
                info.return_type = lower_type(*function.return_type);
            }
            for (const auto& parameter : function.parameters) {
                info.parameter_types.push_back(lower_type(parameter.type));
            }
            if (!functions_.emplace(function.name, std::move(info)).second) {
                throw_backend_error(
                    function.location,
                    "function overloads are not supported by the C backend: '"
                        + function.name + "'");
            }
        }
    }

    CValueType lower_type(const TypeReference& type) const
    {
        if (type.nullable || !type.arguments.empty()) {
            throw_backend_error(
                type.location,
                "type '" + type.name + "' is not supported by the C backend");
        }
        if (type.name == "int") {
            return CValueType::Int;
        }
        if (type.name == "dec") {
            return CValueType::Dec;
        }
        if (type.name == "bool") {
            return CValueType::Bool;
        }
        if (type.name == "string") {
            return CValueType::String;
        }
        throw_backend_error(
            type.location,
            "type '" + type.name + "' is not supported by the C backend");
    }

    std::string function_declaration(
        const FunctionDeclarationStmt& function) const
    {
        const auto& info = functions_.at(function.name);
        std::string output = "static ";
        output += c_type_name(info.return_type) + " " + function_name(function.name) + "(";
        if (function.parameters.empty()) {
            output += "void";
        } else {
            for (std::size_t index = 0; index < function.parameters.size(); ++index) {
                if (index != 0) {
                    output += ", ";
                }
                output += c_type_name(info.parameter_types[index]) + " "
                    + parameter_name(function.parameters[index].name);
            }
        }
        output += ")";
        return output;
    }

    std::string emit_function(const FunctionDeclarationStmt& function)
    {
        current_location_ = function.location;
        scopes_.clear();
        push_scope();
        const auto& info = functions_.at(function.name);
        for (std::size_t index = 0; index < function.parameters.size(); ++index) {
            scopes_.back().emplace(
                function.parameters[index].name,
                ValueInfo{
                    info.parameter_types[index],
                    parameter_name(function.parameters[index].name),
                });
        }

        std::string output = function_declaration(function) + "\n{\n";
        output += emit_statement_list(function.body->statements, 1);
        output += "}\n";
        pop_scope();
        return output;
    }

    std::string emit_statement_list(
        const std::vector<std::unique_ptr<Stmt>>& statements,
        std::size_t depth)
    {
        std::string output;
        for (const auto& statement : statements) {
            output += emit_statement(*statement, depth);
        }
        return output;
    }

    std::string emit_statement(const Stmt& statement, std::size_t depth)
    {
        current_location_ = statement.location;
        const std::string prefix = indent(depth);
        switch (statement.kind) {
        case StmtKind::VariableDeclaration: {
            const auto& declaration =
                static_cast<const VariableDeclarationStmt&>(statement);
            const auto initializer = emit_expression(*declaration.initializer);
            const CValueType type = declaration.explicit_type
                ? lower_type(*declaration.explicit_type)
                : initializer.type;
            const std::string c_name = variable_name(declaration.name);
            scopes_.back().emplace(declaration.name, ValueInfo{type, c_name});
            return prefix + c_type_name(type) + " " + c_name + " = "
                + initializer.code + ";\n";
        }
        case StmtKind::Assignment: {
            const auto& assignment = static_cast<const AssignmentStmt&>(statement);
            const auto value = emit_expression(*assignment.value);
            return prefix + find_value(assignment.name).c_name + " = "
                + value.code + ";\n";
        }
        case StmtKind::Expression: {
            const auto& expression = static_cast<const ExpressionStmt&>(statement);
            return prefix + emit_expression(*expression.expression).code + ";\n";
        }
        case StmtKind::Return: {
            const auto& return_statement = static_cast<const ReturnStmt&>(statement);
            if (!return_statement.value) {
                return prefix + "return;\n";
            }
            return prefix + "return "
                + emit_expression(*return_statement.value).code + ";\n";
        }
        case StmtKind::Block:
            return emit_block(static_cast<const BlockStmt&>(statement), depth);
        case StmtKind::If:
            return emit_if(static_cast<const IfStmt&>(statement), depth);
        case StmtKind::While: {
            const auto& loop = static_cast<const WhileStmt&>(statement);
            std::string output = prefix + "while ("
                + emit_expression(*loop.condition).code + ") ";
            output += emit_braced_block(*loop.body, depth);
            return output;
        }
        case StmtKind::FunctionDeclaration:
            throw_backend_error(
                statement.location,
                "nested functions are not supported by the C backend");
        case StmtKind::MemberAssignment:
        case StmtKind::ForIn:
        case StmtKind::Stop:
        case StmtKind::Continue:
        case StmtKind::EnumDeclaration:
        case StmtKind::Handle:
        case StmtKind::StructDeclaration:
        case StmtKind::ClassDeclaration:
        case StmtKind::InterfaceDeclaration:
            throw_backend_error(
                statement.location,
                "statement is not supported by the C backend");
        }
        throw_backend_error(statement.location, "unknown statement");
    }

    std::string emit_block(const BlockStmt& block, std::size_t depth)
    {
        return indent(depth) + emit_braced_block(block, depth);
    }

    std::string emit_braced_block(const BlockStmt& block, std::size_t depth)
    {
        push_scope();
        std::string output = "{\n";
        output += emit_statement_list(block.statements, depth + 1);
        output += indent(depth) + "}\n";
        pop_scope();
        return output;
    }

    std::string emit_if(const IfStmt& conditional, std::size_t depth)
    {
        std::string output = indent(depth) + "if ("
            + emit_expression(*conditional.condition).code + ") ";
        output += emit_braced_block(*conditional.then_block, depth);
        if (conditional.else_branch) {
            output.resize(output.size() - 1);
            output += " else ";
            if (conditional.else_branch->kind == StmtKind::If) {
                std::string nested = emit_if(
                    static_cast<const IfStmt&>(*conditional.else_branch), depth);
                nested.erase(0, indent(depth).size());
                output += nested;
            } else {
                const auto& block =
                    static_cast<const BlockStmt&>(*conditional.else_branch);
                output += emit_braced_block(block, depth);
            }
        }
        return output;
    }

    GeneratedExpression emit_expression(const Expr& expression)
    {
        switch (expression.kind) {
        case ExprKind::Integer:
            return {static_cast<const IntegerExpr&>(expression).value, CValueType::Int};
        case ExprKind::Decimal:
            return {static_cast<const DecimalExpr&>(expression).value, CValueType::Dec};
        case ExprKind::String:
            return {
                escape_c_string(static_cast<const StringExpr&>(expression).value),
                CValueType::String,
            };
        case ExprKind::Bool:
            return {
                static_cast<const BoolExpr&>(expression).value ? "true" : "false",
                CValueType::Bool,
            };
        case ExprKind::Identifier: {
            const auto& identifier = static_cast<const IdentifierExpr&>(expression);
            const auto& value = find_value(identifier.name);
            return {value.c_name, value.type};
        }
        case ExprKind::Unary: {
            const auto& unary = static_cast<const UnaryExpr&>(expression);
            const auto operand = emit_expression(*unary.operand);
            return {"(-" + operand.code + ")", operand.type};
        }
        case ExprKind::Binary:
            return emit_binary(static_cast<const BinaryExpr&>(expression));
        case ExprKind::Call:
            return emit_call(static_cast<const CallExpr&>(expression));
        case ExprKind::Grouping: {
            const auto inner = emit_expression(
                *static_cast<const GroupingExpr&>(expression).expression);
            return {"(" + inner.code + ")", inner.type};
        }
        case ExprKind::Null:
        case ExprKind::MemberAccess:
        case ExprKind::Propagation:
        case ExprKind::Cast:
            throw_backend_error(
                current_location_,
                "expression is not supported by the C backend");
        }
        throw_backend_error(current_location_, "unknown expression");
    }

    GeneratedExpression emit_binary(const BinaryExpr& binary)
    {
        const auto left = emit_expression(*binary.left);
        const auto right = emit_expression(*binary.right);
        if ((binary.operator_token.type == TokenType::Equal
                || binary.operator_token.type == TokenType::NotEqual)
            && left.type == CValueType::String) {
            const std::string comparison = binary.operator_token.type
                    == TokenType::Equal
                ? " == 0"
                : " != 0";
            return {
                "(strcmp(" + left.code + ", " + right.code + ")" + comparison + ")",
                CValueType::Bool,
            };
        }

        std::string operation;
        CValueType result_type = left.type;
        switch (binary.operator_token.type) {
        case TokenType::Plus: operation = "+"; break;
        case TokenType::Minus: operation = "-"; break;
        case TokenType::Star: operation = "*"; break;
        case TokenType::Slash: operation = "/"; break;
        case TokenType::Equal: operation = "=="; result_type = CValueType::Bool; break;
        case TokenType::NotEqual: operation = "!="; result_type = CValueType::Bool; break;
        case TokenType::Less: operation = "<"; result_type = CValueType::Bool; break;
        case TokenType::LessEqual: operation = "<="; result_type = CValueType::Bool; break;
        case TokenType::Greater: operation = ">"; result_type = CValueType::Bool; break;
        case TokenType::GreaterEqual: operation = ">="; result_type = CValueType::Bool; break;
        case TokenType::And: operation = "&&"; result_type = CValueType::Bool; break;
        case TokenType::Or: operation = "||"; result_type = CValueType::Bool; break;
        default:
            throw_backend_error(
                current_location_, "operator '" + binary.operator_token.lexeme
                    + "' is not supported by the C backend");
        }
        return {
            "(" + left.code + " " + operation + " " + right.code + ")",
            result_type,
        };
    }

    GeneratedExpression emit_call(const CallExpr& call)
    {
        if (!call.generic_arguments.empty()) {
            throw_backend_error(
                current_location_,
                "generic calls are not supported by the C backend");
        }
        if (call.callee->kind != ExprKind::Identifier) {
            throw_backend_error(
                current_location_,
                "only direct function calls are supported by the C backend");
        }
        const auto& callee = static_cast<const IdentifierExpr&>(*call.callee);
        if (callee.name == "print") {
            return emit_print(call);
        }
        const auto function = functions_.find(callee.name);
        if (function == functions_.end()) {
            throw_backend_error(
                current_location_, "unknown backend function '" + callee.name + "'");
        }

        std::vector<const CallArgument*> ordered(call.arguments.size(), nullptr);
        for (std::size_t index = 0; index < call.arguments.size(); ++index) {
            const auto& argument = call.arguments[index];
            if (!argument.name) {
                ordered[index] = &argument;
                continue;
            }
            const auto parameter = std::find_if(
                function->second.declaration->parameters.begin(),
                function->second.declaration->parameters.end(),
                [&](const Parameter& candidate) {
                    return candidate.name == *argument.name;
                });
            const auto parameter_index = static_cast<std::size_t>(
                parameter - function->second.declaration->parameters.begin());
            ordered[parameter_index] = &argument;
        }

        std::string code = function_name(callee.name) + "(";
        for (std::size_t index = 0; index < ordered.size(); ++index) {
            if (index != 0) {
                code += ", ";
            }
            code += emit_expression(*ordered[index]->value).code;
        }
        code += ")";
        return {std::move(code), function->second.return_type};
    }

    GeneratedExpression emit_print(const CallExpr& call)
    {
        const auto value = emit_expression(*call.arguments.front().value);
        switch (value.type) {
        case CValueType::Int:
            return {
                "printf(\"%lld\\n\", (long long)(" + value.code + "))",
                CValueType::Void,
            };
        case CValueType::Dec:
            return {"printf(\"%g\\n\", " + value.code + ")", CValueType::Void};
        case CValueType::Bool:
            return {
                "printf(\"%s\\n\", (" + value.code
                    + ") ? \"true\" : \"false\")",
                CValueType::Void,
            };
        case CValueType::String:
            return {"printf(\"%s\\n\", " + value.code + ")", CValueType::Void};
        case CValueType::Void:
            throw_backend_error(
                current_location_, "cannot print an expression without a value");
        }
        throw_backend_error(current_location_, "unsupported print value");
    }

    void push_scope()
    {
        scopes_.emplace_back();
    }

    void pop_scope()
    {
        scopes_.pop_back();
    }

    const ValueInfo& find_value(const std::string& name) const
    {
        for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope) {
            if (const auto value = scope->find(name); value != scope->end()) {
                return value->second;
            }
        }
        throw_backend_error(
            current_location_, "cannot resolve value '" + name + "' in C backend");
    }

    std::unordered_map<std::string, FunctionInfo> functions_;
    std::vector<std::unordered_map<std::string, ValueInfo>> scopes_;
    SourceLocation current_location_{1, 1};
};

} // namespace

std::string CGenerator::generate(const Program& program)
{
    return Generator{}.generate(program);
}

} // namespace toro
