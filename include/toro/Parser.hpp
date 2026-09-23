#pragma once

#include "toro/AST.hpp"
#include "toro/Token.hpp"

#include <cstddef>
#include <initializer_list>
#include <memory>
#include <vector>

namespace toro {

class Parser {
public:
    explicit Parser(std::vector<Token> tokens);

    [[nodiscard]] std::unique_ptr<Expr> parse_expression();

private:
    [[nodiscard]] std::unique_ptr<Expr> parse_equality();
    [[nodiscard]] std::unique_ptr<Expr> parse_comparison();
    [[nodiscard]] std::unique_ptr<Expr> parse_term();
    [[nodiscard]] std::unique_ptr<Expr> parse_factor();
    [[nodiscard]] std::unique_ptr<Expr> parse_unary();
    [[nodiscard]] std::unique_ptr<Expr> parse_primary();

    [[nodiscard]] bool at_end() const;
    [[nodiscard]] bool check(TokenType type) const;
    bool match(std::initializer_list<TokenType> types);
    const Token& advance();
    [[nodiscard]] const Token& peek() const;
    [[nodiscard]] const Token& previous() const;
    const Token& consume(TokenType type, const char* message);

    std::vector<Token> tokens_;
    std::size_t current_{0};
};

} // namespace toro
