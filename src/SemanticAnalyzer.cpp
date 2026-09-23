#include "toro/SemanticAnalyzer.hpp"

#include <stdexcept>
#include <string>

namespace toro {
namespace {

[[noreturn]] void throw_semantic_error(SourceLocation location, const std::string& message)
{
    throw std::runtime_error(
        "line " + std::to_string(location.line) + ", column "
        + std::to_string(location.column) + ": semantic error: " + message);
}

} // namespace

void SemanticAnalyzer::analyze(const Program& program)
{
    scopes_.clear();
    inside_class_method_ = false;
    push_scope();
    scopes_.back().emplace("print", Symbol{SymbolKind::Builtin, SourceLocation{0, 0}});
    analyze_statement_list(program.statements);
    pop_scope();
}

void SemanticAnalyzer::analyze_statement_list(
    const std::vector<std::unique_ptr<Stmt>>& statements)
{
    predeclare(statements);
    for (const auto& statement : statements) {
        analyze_statement(*statement);
    }
}

void SemanticAnalyzer::predeclare(const std::vector<std::unique_ptr<Stmt>>& statements)
{
    for (const auto& statement : statements) {
        switch (statement->kind) {
        case StmtKind::FunctionDeclaration: {
            const auto& function = static_cast<const FunctionDeclarationStmt&>(*statement);
            declare(function.name, SymbolKind::Function, function.location);
            break;
        }
        case StmtKind::StructDeclaration: {
            const auto& declaration = static_cast<const StructDeclarationStmt&>(*statement);
            declare(declaration.name, SymbolKind::Struct, declaration.location);
            break;
        }
        case StmtKind::ClassDeclaration: {
            const auto& declaration = static_cast<const ClassDeclarationStmt&>(*statement);
            declare(declaration.name, SymbolKind::Class, declaration.location);
            break;
        }
        case StmtKind::InterfaceDeclaration: {
            const auto& declaration = static_cast<const InterfaceDeclarationStmt&>(*statement);
            declare(declaration.name, SymbolKind::Interface, declaration.location);
            break;
        }
        case StmtKind::EnumDeclaration: {
            const auto& declaration = static_cast<const EnumDeclarationStmt&>(*statement);
            declare(declaration.name, SymbolKind::Enum, declaration.location);
            break;
        }
        default:
            break;
        }
    }
}

void SemanticAnalyzer::analyze_statement(const Stmt& statement)
{
    current_location_ = statement.location;
    switch (statement.kind) {
    case StmtKind::VariableDeclaration: {
        const auto& declaration = static_cast<const VariableDeclarationStmt&>(statement);
        analyze_expression(*declaration.initializer);
        declare(declaration.name, SymbolKind::Variable, declaration.location);
        return;
    }
    case StmtKind::Assignment: {
        const auto& assignment = static_cast<const AssignmentStmt&>(statement);
        if (!resolve(assignment.name)) {
            throw_semantic_error(
                assignment.location, "unknown identifier '" + assignment.name + "'");
        }
        analyze_expression(*assignment.value);
        return;
    }
    case StmtKind::MemberAssignment: {
        const auto& assignment = static_cast<const MemberAssignmentStmt&>(statement);
        analyze_expression(*assignment.target);
        analyze_expression(*assignment.value);
        return;
    }
    case StmtKind::Expression:
        analyze_expression(*static_cast<const ExpressionStmt&>(statement).expression);
        return;
    case StmtKind::FunctionDeclaration:
        analyze_function(static_cast<const FunctionDeclarationStmt&>(statement));
        return;
    case StmtKind::Return: {
        const auto& return_statement = static_cast<const ReturnStmt&>(statement);
        if (return_statement.value) {
            analyze_expression(*return_statement.value);
        }
        return;
    }
    case StmtKind::Block:
        analyze_block(static_cast<const BlockStmt&>(statement));
        return;
    case StmtKind::If: {
        const auto& if_statement = static_cast<const IfStmt&>(statement);
        analyze_expression(*if_statement.condition);
        analyze_block(*if_statement.then_block);
        if (if_statement.else_branch) {
            analyze_statement(*if_statement.else_branch);
        }
        return;
    }
    case StmtKind::While: {
        const auto& loop = static_cast<const WhileStmt&>(statement);
        analyze_expression(*loop.condition);
        analyze_block(*loop.body);
        return;
    }
    case StmtKind::ForIn: {
        const auto& loop = static_cast<const ForInStmt&>(statement);
        analyze_expression(*loop.collection);
        push_scope();
        declare(loop.variable_name, SymbolKind::LoopVariable, loop.location);
        analyze_statement_list(loop.body->statements);
        pop_scope();
        return;
    }
    case StmtKind::Stop:
    case StmtKind::Continue:
        return;
    case StmtKind::EnumDeclaration:
    case StmtKind::InterfaceDeclaration:
        return;
    case StmtKind::Handle: {
        const auto& handle = static_cast<const HandleStmt&>(statement);
        analyze_expression(*handle.expression);
        for (const auto& handle_case : handle.cases) {
            push_scope();
            if (handle_case.binding_name) {
                declare(
                    *handle_case.binding_name,
                    SymbolKind::HandleBinding,
                    handle_case.location);
            }
            analyze_statement_list(handle_case.body->statements);
            pop_scope();
        }
        return;
    }
    case StmtKind::StructDeclaration: {
        const auto& declaration = static_cast<const StructDeclarationStmt&>(statement);
        for (const auto& field : declaration.fields) {
            if (field.default_value) {
                current_location_ = field.location;
                analyze_expression(*field.default_value);
            }
        }
        for (const auto& conversion : declaration.conversions) {
            analyze_conversion(*conversion);
        }
        return;
    }
    case StmtKind::ClassDeclaration: {
        const auto& declaration = static_cast<const ClassDeclarationStmt&>(statement);
        for (const auto& member : declaration.members) {
            current_location_ = member->location;
            if (member->kind == ClassMemberKind::Field) {
                const auto& field = static_cast<const ClassField&>(*member);
                if (field.default_value) {
                    analyze_expression(*field.default_value);
                }
            } else if (member->kind == ClassMemberKind::Method) {
                analyze_method(static_cast<const MethodDeclaration&>(*member));
            } else {
                analyze_conversion(static_cast<const ConversionOverload&>(*member));
            }
        }
        return;
    }
    }
}

void SemanticAnalyzer::analyze_expression(const Expr& expression)
{
    switch (expression.kind) {
    case ExprKind::Integer:
    case ExprKind::Decimal:
    case ExprKind::String:
    case ExprKind::Bool:
    case ExprKind::Null:
        return;
    case ExprKind::Identifier: {
        const auto& identifier = static_cast<const IdentifierExpr&>(expression);
        if (identifier.name == "self") {
            if (!inside_class_method_) {
                throw_semantic_error(
                    current_location_, "'self' is only valid inside class methods");
            }
            return;
        }
        if (!resolve(identifier.name)) {
            throw_semantic_error(
                current_location_, "unknown identifier '" + identifier.name + "'");
        }
        return;
    }
    case ExprKind::Unary:
        analyze_expression(*static_cast<const UnaryExpr&>(expression).operand);
        return;
    case ExprKind::Binary: {
        const auto& binary = static_cast<const BinaryExpr&>(expression);
        analyze_expression(*binary.left);
        analyze_expression(*binary.right);
        return;
    }
    case ExprKind::Call: {
        const auto& call = static_cast<const CallExpr&>(expression);
        analyze_expression(*call.callee);
        for (const auto& argument : call.arguments) {
            analyze_expression(*argument.value);
        }
        return;
    }
    case ExprKind::MemberAccess:
        analyze_expression(*static_cast<const MemberAccessExpr&>(expression).object);
        return;
    case ExprKind::Cast:
        analyze_expression(*static_cast<const CastExpr&>(expression).expression);
        return;
    case ExprKind::Grouping:
        analyze_expression(*static_cast<const GroupingExpr&>(expression).expression);
        return;
    }
}

void SemanticAnalyzer::analyze_function(const FunctionDeclarationStmt& function)
{
    const bool enclosing_class_method = inside_class_method_;
    inside_class_method_ = false;
    push_scope();
    for (const auto& parameter : function.parameters) {
        declare(parameter.name, SymbolKind::Parameter, parameter.location);
    }
    analyze_statement_list(function.body->statements);
    pop_scope();
    inside_class_method_ = enclosing_class_method;
}

void SemanticAnalyzer::analyze_method(const MethodDeclaration& method)
{
    if (!method.body) {
        return;
    }

    const bool enclosing_class_method = inside_class_method_;
    inside_class_method_ = true;
    push_scope();
    for (const auto& parameter : method.parameters) {
        declare(parameter.name, SymbolKind::Parameter, parameter.location);
    }
    analyze_statement_list(method.body->statements);
    pop_scope();
    inside_class_method_ = enclosing_class_method;
}

void SemanticAnalyzer::analyze_conversion(const ConversionOverload& conversion)
{
    const bool enclosing_class_method = inside_class_method_;
    inside_class_method_ = true;
    push_scope();
    analyze_statement_list(conversion.body->statements);
    pop_scope();
    inside_class_method_ = enclosing_class_method;
}

void SemanticAnalyzer::analyze_block(const BlockStmt& block)
{
    push_scope();
    analyze_statement_list(block.statements);
    pop_scope();
}

void SemanticAnalyzer::push_scope()
{
    scopes_.emplace_back();
}

void SemanticAnalyzer::pop_scope()
{
    scopes_.pop_back();
}

void SemanticAnalyzer::declare(
    const std::string& name,
    SymbolKind kind,
    SourceLocation location)
{
    auto& scope = scopes_.back();
    if (scope.contains(name)) {
        throw_semantic_error(location, "duplicate declaration '" + name + "' in this scope");
    }
    scope.emplace(name, Symbol{kind, location});
}

bool SemanticAnalyzer::resolve(const std::string& name) const
{
    for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope) {
        if (scope->contains(name)) {
            return true;
        }
    }
    return false;
}

} // namespace toro
