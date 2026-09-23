#pragma once

#include "toro/Token.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace toro {

enum class ExprKind {
    Integer,
    Decimal,
    String,
    Bool,
    Null,
    Identifier,
    Unary,
    Binary,
    Call,
    Grouping,
};

struct Expr {
    explicit Expr(ExprKind kind)
        : kind(kind)
    {
    }

    virtual ~Expr() = default;

    ExprKind kind;
};

struct IntegerExpr final : Expr {
    explicit IntegerExpr(std::string value)
        : Expr(ExprKind::Integer)
        , value(std::move(value))
    {
    }

    std::string value;
};

struct DecimalExpr final : Expr {
    explicit DecimalExpr(std::string value)
        : Expr(ExprKind::Decimal)
        , value(std::move(value))
    {
    }

    std::string value;
};

struct StringExpr final : Expr {
    explicit StringExpr(std::string value)
        : Expr(ExprKind::String)
        , value(std::move(value))
    {
    }

    std::string value;
};

struct BoolExpr final : Expr {
    explicit BoolExpr(bool value)
        : Expr(ExprKind::Bool)
        , value(value)
    {
    }

    bool value;
};

struct NullExpr final : Expr {
    NullExpr()
        : Expr(ExprKind::Null)
    {
    }
};

struct IdentifierExpr final : Expr {
    explicit IdentifierExpr(std::string name)
        : Expr(ExprKind::Identifier)
        , name(std::move(name))
    {
    }

    std::string name;
};

struct UnaryExpr final : Expr {
    UnaryExpr(Token operator_token, std::unique_ptr<Expr> operand)
        : Expr(ExprKind::Unary)
        , operator_token(std::move(operator_token))
        , operand(std::move(operand))
    {
    }

    Token operator_token;
    std::unique_ptr<Expr> operand;
};

struct BinaryExpr final : Expr {
    BinaryExpr(
        std::unique_ptr<Expr> left,
        Token operator_token,
        std::unique_ptr<Expr> right)
        : Expr(ExprKind::Binary)
        , left(std::move(left))
        , operator_token(std::move(operator_token))
        , right(std::move(right))
    {
    }

    std::unique_ptr<Expr> left;
    Token operator_token;
    std::unique_ptr<Expr> right;
};

struct CallExpr final : Expr {
    CallExpr(std::unique_ptr<Expr> callee, std::vector<std::unique_ptr<Expr>> arguments)
        : Expr(ExprKind::Call)
        , callee(std::move(callee))
        , arguments(std::move(arguments))
    {
    }

    std::unique_ptr<Expr> callee;
    std::vector<std::unique_ptr<Expr>> arguments;
};

struct GroupingExpr final : Expr {
    explicit GroupingExpr(std::unique_ptr<Expr> expression)
        : Expr(ExprKind::Grouping)
        , expression(std::move(expression))
    {
    }

    std::unique_ptr<Expr> expression;
};

struct SourceLocation {
    std::size_t line;
    std::size_t column;
};

enum class StmtKind {
    VariableDeclaration,
    Assignment,
    Expression,
    FunctionDeclaration,
    Return,
    Block,
};

struct Stmt {
    Stmt(StmtKind kind, SourceLocation location)
        : kind(kind)
        , location(location)
    {
    }

    virtual ~Stmt() = default;

    StmtKind kind;
    SourceLocation location;
};

struct VariableDeclarationStmt final : Stmt {
    VariableDeclarationStmt(
        SourceLocation location,
        std::string name,
        std::optional<std::string> explicit_type,
        std::unique_ptr<Expr> initializer)
        : Stmt(StmtKind::VariableDeclaration, location)
        , name(std::move(name))
        , explicit_type(std::move(explicit_type))
        , initializer(std::move(initializer))
    {
    }

    std::string name;
    std::optional<std::string> explicit_type;
    std::unique_ptr<Expr> initializer;
};

struct AssignmentStmt final : Stmt {
    AssignmentStmt(
        SourceLocation location,
        std::string name,
        std::unique_ptr<Expr> value)
        : Stmt(StmtKind::Assignment, location)
        , name(std::move(name))
        , value(std::move(value))
    {
    }

    std::string name;
    std::unique_ptr<Expr> value;
};

struct ExpressionStmt final : Stmt {
    ExpressionStmt(SourceLocation location, std::unique_ptr<Expr> expression)
        : Stmt(StmtKind::Expression, location)
        , expression(std::move(expression))
    {
    }

    std::unique_ptr<Expr> expression;
};

struct Parameter {
    std::string name;
    std::string type;
    SourceLocation location;
};

struct BlockStmt final : Stmt {
    BlockStmt(SourceLocation location, std::vector<std::unique_ptr<Stmt>> statements)
        : Stmt(StmtKind::Block, location)
        , statements(std::move(statements))
    {
    }

    std::vector<std::unique_ptr<Stmt>> statements;
};

struct FunctionDeclarationStmt final : Stmt {
    FunctionDeclarationStmt(
        SourceLocation location,
        std::string name,
        std::vector<Parameter> parameters,
        std::optional<std::string> return_type,
        std::unique_ptr<BlockStmt> body)
        : Stmt(StmtKind::FunctionDeclaration, location)
        , name(std::move(name))
        , parameters(std::move(parameters))
        , return_type(std::move(return_type))
        , body(std::move(body))
    {
    }

    std::string name;
    std::vector<Parameter> parameters;
    std::optional<std::string> return_type;
    std::unique_ptr<BlockStmt> body;
};

struct ReturnStmt final : Stmt {
    ReturnStmt(SourceLocation location, std::unique_ptr<Expr> value)
        : Stmt(StmtKind::Return, location)
        , value(std::move(value))
    {
    }

    std::unique_ptr<Expr> value;
};

struct Program {
    std::vector<std::unique_ptr<Stmt>> statements;
};

[[nodiscard]] std::string dump_expression(const Expr& expression);
[[nodiscard]] std::string dump_program(const Program& program);

} // namespace toro
