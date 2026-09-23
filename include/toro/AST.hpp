#pragma once

#include "toro/Token.hpp"

#include <memory>
#include <string>
#include <utility>

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

struct GroupingExpr final : Expr {
    explicit GroupingExpr(std::unique_ptr<Expr> expression)
        : Expr(ExprKind::Grouping)
        , expression(std::move(expression))
    {
    }

    std::unique_ptr<Expr> expression;
};

[[nodiscard]] std::string dump_expression(const Expr& expression);

} // namespace toro
