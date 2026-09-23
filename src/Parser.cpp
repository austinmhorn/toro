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
    case ExprKind::Call: {
        const auto& call = static_cast<const CallExpr&>(expression);
        output += "Call\n";
        append_dump(*call.callee, depth + 1, output);
        for (const auto& argument : call.arguments) {
            append_dump(*argument, depth + 1, output);
        }
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

void append_statement_dump(const Stmt& statement, std::string& output)
{
    switch (statement.kind) {
    case StmtKind::VariableDeclaration: {
        const auto& declaration = static_cast<const VariableDeclarationStmt&>(statement);
        output += "VariableDeclaration(" + declaration.name + ")\n  ";
        if (declaration.explicit_type) {
            output += "type: " + *declaration.explicit_type + "\n";
        } else {
            output += "inferred\n";
        }
        append_dump(*declaration.initializer, 1, output);
        return;
    }
    case StmtKind::Assignment: {
        const auto& assignment = static_cast<const AssignmentStmt&>(statement);
        output += "Assignment(" + assignment.name + ")\n";
        append_dump(*assignment.value, 1, output);
        return;
    }
    case StmtKind::Expression: {
        const auto& expression = static_cast<const ExpressionStmt&>(statement);
        output += "ExpressionStatement\n";
        append_dump(*expression.expression, 1, output);
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
    statement_line_ = peek().line;
    auto expression = parse_equality();
    if (!at_end()) {
        throw_parse_error(peek(), "unexpected token after expression");
    }
    return expression;
}

Program Parser::parse_program()
{
    Program program;
    while (!at_end()) {
        statement_line_ = peek().line;
        program.statements.push_back(parse_statement());
    }
    statement_line_.reset();
    return program;
}

std::unique_ptr<Stmt> Parser::parse_statement()
{
    if (check(TokenType::Identifier)
        && (check_next(TokenType::Declare) || check_next(TokenType::Colon))) {
        return parse_variable_declaration();
    }

    const SourceLocation location{peek().line, peek().column};
    auto expression = parse_equality();

    if (match({TokenType::Assign})) {
        const Token assignment = previous();
        if (expression->kind != ExprKind::Identifier) {
            throw_parse_error(assignment, "invalid assignment target; expected identifier");
        }

        require_expression("expected assignment value");
        auto value = parse_equality();
        require_statement_end();

        auto name = std::move(static_cast<IdentifierExpr&>(*expression).name);
        return std::make_unique<AssignmentStmt>(location, std::move(name), std::move(value));
    }

    require_statement_end();
    return std::make_unique<ExpressionStmt>(location, std::move(expression));
}

std::unique_ptr<Stmt> Parser::parse_variable_declaration()
{
    Token name = advance();
    const SourceLocation location{name.line, name.column};
    std::optional<std::string> explicit_type;

    if (!match({TokenType::Declare})) {
        consume(TokenType::Colon, "expected ':' after variable name");
        const Token& type_name = consume(TokenType::Identifier, "expected type name after ':'");
        explicit_type = type_name.lexeme;
        consume(TokenType::Assign, "expected '=' and initializer after type name");
    }

    require_expression("expected variable initializer");
    auto initializer = parse_equality();
    require_statement_end();

    return std::make_unique<VariableDeclarationStmt>(
        location, std::move(name.lexeme), std::move(explicit_type), std::move(initializer));
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

    return parse_call();
}

std::unique_ptr<Expr> Parser::parse_call()
{
    auto expression = parse_primary();

    while (match({TokenType::LeftParen})) {
        expression = finish_call(std::move(expression));
    }

    return expression;
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

std::unique_ptr<Expr> Parser::finish_call(std::unique_ptr<Expr> callee)
{
    std::vector<std::unique_ptr<Expr>> arguments;
    if (!check(TokenType::RightParen)) {
        do {
            require_expression("expected call argument");
            arguments.push_back(parse_equality());
        } while (match({TokenType::Comma}));
    }

    consume(TokenType::RightParen, "expected ')' after call arguments");
    return std::make_unique<CallExpr>(std::move(callee), std::move(arguments));
}

bool Parser::at_end() const
{
    return peek().type == TokenType::EndOfFile;
}

bool Parser::check(TokenType type) const
{
    if (peek().type != type) {
        return false;
    }
    return type == TokenType::EndOfFile
        || !statement_line_
        || peek().line == *statement_line_;
}

bool Parser::check_next(TokenType type) const
{
    if (current_ + 1 >= tokens_.size()) {
        return false;
    }
    const auto& token = tokens_[current_ + 1];
    return token.type == type
        && (!statement_line_ || token.line == *statement_line_);
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

void Parser::require_expression(const char* message) const
{
    if (at_end() || (statement_line_ && peek().line != *statement_line_)) {
        throw_parse_error(peek(), message);
    }
}

void Parser::require_statement_end() const
{
    if (!at_end() && statement_line_ && peek().line == *statement_line_) {
        throw_parse_error(peek(), "unexpected token after statement");
    }
}

std::string dump_expression(const Expr& expression)
{
    std::string output;
    append_dump(expression, 0, output);
    return output;
}

std::string dump_program(const Program& program)
{
    std::string output;
    for (std::size_t index = 0; index < program.statements.size(); ++index) {
        append_statement_dump(*program.statements[index], output);
        if (index + 1 < program.statements.size()) {
            output += '\n';
        }
    }
    return output;
}

} // namespace toro
