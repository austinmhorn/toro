#include "toro/Parser.hpp"

#include <stdexcept>
#include <string>
#include <utility>

namespace toro {
namespace {

[[noreturn]] void throw_parse_error(const Token& token, const char* message)
{
    throw std::runtime_error(
        "line " + std::to_string(token.line) + ", column "
        + std::to_string(token.column) + ": " + message);
}

void append_dump(const Expr& expression, std::size_t depth, std::string& output)
{
    output.append(depth * 2, ' ');

    switch (expression.kind) {
    case ExprKind::Integer:
        output += "Integer(" + static_cast<const IntegerExpr&>(expression).value + ")\n";
        return;
    case ExprKind::Decimal:
        output += "Decimal(" + static_cast<const DecimalExpr&>(expression).value + ")\n";
        return;
    case ExprKind::String:
        output += "String(" + static_cast<const StringExpr&>(expression).value + ")\n";
        return;
    case ExprKind::Bool:
        output += static_cast<const BoolExpr&>(expression).value ? "Bool(true)\n" : "Bool(false)\n";
        return;
    case ExprKind::Null:
        output += "Null\n";
        return;
    case ExprKind::Identifier:
        output += "Identifier(" + static_cast<const IdentifierExpr&>(expression).name + ")\n";
        return;
    case ExprKind::Unary: {
        const auto& unary = static_cast<const UnaryExpr&>(expression);
        output += "Unary(" + unary.operator_token.lexeme + ")\n";
        append_dump(*unary.operand, depth + 1, output);
        return;
    }
    case ExprKind::Binary: {
        const auto& binary = static_cast<const BinaryExpr&>(expression);
        output += "Binary(" + binary.operator_token.lexeme + ")\n";
        append_dump(*binary.left, depth + 1, output);
        append_dump(*binary.right, depth + 1, output);
        return;
    }
    case ExprKind::Grouping: {
        const auto& grouping = static_cast<const GroupingExpr&>(expression);
        output += "Grouping\n";
        append_dump(*grouping.expression, depth + 1, output);
        return;
    }
    }
}

} // namespace

Parser::Parser(std::vector<Token> tokens)
    : tokens_(std::move(tokens))
{
    if (tokens_.empty() || tokens_.back().type != TokenType::EndOfFile) {
        throw std::invalid_argument("parser requires a token stream ending in EOF");
    }
}

std::unique_ptr<Expr> Parser::parse_expression()
{
    auto expression = parse_equality();
    if (!at_end()) {
        throw_parse_error(peek(), "unexpected token after expression");
    }
    return expression;
}

std::unique_ptr<Expr> Parser::parse_equality()
{
    auto expression = parse_comparison();

    while (match({TokenType::Equal, TokenType::NotEqual})) {
        Token operator_token = previous();
        auto right = parse_comparison();
        expression = std::make_unique<BinaryExpr>(
            std::move(expression), std::move(operator_token), std::move(right));
    }

    return expression;
}

std::unique_ptr<Expr> Parser::parse_comparison()
{
    auto expression = parse_term();

    while (match({
        TokenType::Less,
        TokenType::LessEqual,
        TokenType::Greater,
        TokenType::GreaterEqual,
    })) {
        Token operator_token = previous();
        auto right = parse_term();
        expression = std::make_unique<BinaryExpr>(
            std::move(expression), std::move(operator_token), std::move(right));
    }

    return expression;
}

std::unique_ptr<Expr> Parser::parse_term()
{
    auto expression = parse_factor();

    while (match({TokenType::Plus, TokenType::Minus})) {
        Token operator_token = previous();
        auto right = parse_factor();
        expression = std::make_unique<BinaryExpr>(
            std::move(expression), std::move(operator_token), std::move(right));
    }

    return expression;
}

std::unique_ptr<Expr> Parser::parse_factor()
{
    auto expression = parse_unary();

    while (match({TokenType::Star, TokenType::Slash})) {
        Token operator_token = previous();
        auto right = parse_unary();
        expression = std::make_unique<BinaryExpr>(
            std::move(expression), std::move(operator_token), std::move(right));
    }

    return expression;
}

std::unique_ptr<Expr> Parser::parse_unary()
{
    if (match({TokenType::Minus})) {
        Token operator_token = previous();
        return std::make_unique<UnaryExpr>(
            std::move(operator_token), parse_unary());
    }

    return parse_primary();
}

std::unique_ptr<Expr> Parser::parse_primary()
{
    if (match({TokenType::Integer})) {
        return std::make_unique<IntegerExpr>(previous().lexeme);
    }
    if (match({TokenType::Decimal})) {
        return std::make_unique<DecimalExpr>(previous().lexeme);
    }
    if (match({TokenType::String})) {
        return std::make_unique<StringExpr>(previous().lexeme);
    }
    if (match({TokenType::True})) {
        return std::make_unique<BoolExpr>(true);
    }
    if (match({TokenType::False})) {
        return std::make_unique<BoolExpr>(false);
    }
    if (match({TokenType::Null})) {
        return std::make_unique<NullExpr>();
    }
    if (match({TokenType::Identifier})) {
        return std::make_unique<IdentifierExpr>(previous().lexeme);
    }
    if (match({TokenType::LeftParen})) {
        auto expression = parse_equality();
        consume(TokenType::RightParen, "expected ')' after expression");
        return std::make_unique<GroupingExpr>(std::move(expression));
    }

    throw_parse_error(peek(), "expected expression");
}

bool Parser::at_end() const
{
    return peek().type == TokenType::EndOfFile;
}

bool Parser::check(TokenType type) const
{
    return peek().type == type;
}

bool Parser::match(std::initializer_list<TokenType> types)
{
    for (const auto type : types) {
        if (check(type)) {
            advance();
            return true;
        }
    }
    return false;
}

const Token& Parser::advance()
{
    if (!at_end()) {
        ++current_;
    }
    return previous();
}

const Token& Parser::peek() const
{
    return tokens_[current_];
}

const Token& Parser::previous() const
{
    return tokens_[current_ - 1];
}

const Token& Parser::consume(TokenType type, const char* message)
{
    if (check(type)) {
        return advance();
    }
    throw_parse_error(peek(), message);
}

std::string dump_expression(const Expr& expression)
{
    std::string output;
    append_dump(expression, 0, output);
    return output;
}

} // namespace toro
