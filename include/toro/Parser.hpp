#pragma once

#include "toro/AST.hpp"
#include "toro/Token.hpp"

#include <cstddef>
#include <initializer_list>
#include <memory>
#include <optional>
#include <vector>

namespace toro {

class Parser {
public:
    explicit Parser(std::vector<Token> tokens);

    [[nodiscard]] std::unique_ptr<Expr> parse_expression();
    [[nodiscard]] Program parse_program();

private:
    [[nodiscard]] std::unique_ptr<Stmt> parse_statement();
    [[nodiscard]] std::unique_ptr<Stmt> parse_variable_declaration();
    [[nodiscard]] std::unique_ptr<Stmt> parse_function_declaration();
    [[nodiscard]] std::unique_ptr<Stmt> parse_return_statement();
    [[nodiscard]] std::unique_ptr<Stmt> parse_if_statement();
    [[nodiscard]] std::unique_ptr<Stmt> parse_while_statement();
    [[nodiscard]] std::unique_ptr<Stmt> parse_for_in_statement();
    [[nodiscard]] std::unique_ptr<Stmt> parse_loop_control_statement();
    [[nodiscard]] std::unique_ptr<BlockStmt> parse_block_statement();
    [[nodiscard]] std::unique_ptr<Expr> parse_or();
    [[nodiscard]] std::unique_ptr<Expr> parse_and();
    [[nodiscard]] std::unique_ptr<Expr> parse_equality();
    [[nodiscard]] std::unique_ptr<Expr> parse_comparison();
    [[nodiscard]] std::unique_ptr<Expr> parse_term();
    [[nodiscard]] std::unique_ptr<Expr> parse_factor();
    [[nodiscard]] std::unique_ptr<Expr> parse_unary();
    [[nodiscard]] std::unique_ptr<Expr> parse_call();
    [[nodiscard]] std::unique_ptr<Expr> parse_primary();
    [[nodiscard]] std::unique_ptr<Expr> finish_call(std::unique_ptr<Expr> callee);

    [[nodiscard]] bool at_end() const;
    [[nodiscard]] bool check(TokenType type) const;
    [[nodiscard]] bool check_next(TokenType type) const;
    bool match(std::initializer_list<TokenType> types);
    const Token& advance();
    [[nodiscard]] const Token& peek() const;
    [[nodiscard]] const Token& previous() const;
    const Token& consume(TokenType type, const char* message);
    void require_expression(const char* message) const;
    void require_statement_end() const;

    std::vector<Token> tokens_;
    std::size_t current_{0};
    std::size_t loop_depth_{0};
    std::optional<std::size_t> statement_line_;
};

} // namespace toro
