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
    [[nodiscard]] std::unique_ptr<Stmt> parse_enum_declaration();
    [[nodiscard]] std::unique_ptr<Stmt> parse_handle_statement();
    [[nodiscard]] std::unique_ptr<Stmt> parse_struct_declaration();
    [[nodiscard]] std::unique_ptr<Stmt> parse_class_declaration(bool is_abstract);
    [[nodiscard]] std::unique_ptr<Stmt> parse_interface_declaration();
    [[nodiscard]] std::vector<TypeReference> parse_interface_list();
    [[nodiscard]] std::vector<GenericParameter> parse_generic_parameters();
    [[nodiscard]] TypeReference parse_type_reference(
        const char* message = "expected type name");
    [[nodiscard]] std::unique_ptr<ClassMember> parse_class_field(Visibility visibility);
    [[nodiscard]] std::unique_ptr<ClassMember> parse_method_declaration(
        Visibility visibility,
        bool is_virtual,
        bool is_override,
        bool& saw_destroy);
    [[nodiscard]] std::unique_ptr<ConversionOverload> parse_conversion_overload();
    [[nodiscard]] std::unique_ptr<BlockStmt> parse_block_statement();
    [[nodiscard]] std::unique_ptr<Expr> parse_or();
    [[nodiscard]] std::unique_ptr<Expr> parse_and();
    [[nodiscard]] std::unique_ptr<Expr> parse_equality();
    [[nodiscard]] std::unique_ptr<Expr> parse_comparison();
    [[nodiscard]] std::unique_ptr<Expr> parse_term();
    [[nodiscard]] std::unique_ptr<Expr> parse_factor();
    [[nodiscard]] std::unique_ptr<Expr> parse_unary();
    [[nodiscard]] std::unique_ptr<Expr> parse_cast();
    [[nodiscard]] std::unique_ptr<Expr> parse_call();
    [[nodiscard]] std::unique_ptr<Expr> parse_primary();
    [[nodiscard]] std::unique_ptr<Expr> finish_call(
        std::unique_ptr<Expr> callee,
        std::vector<TypeReference> generic_arguments = {});
    [[nodiscard]] bool looks_like_generic_call() const;
    [[nodiscard]] bool scan_type_reference(std::size_t& index) const;

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
