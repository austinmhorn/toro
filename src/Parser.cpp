#include "toro/Parser.hpp"

#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>

namespace toro {
namespace {

[[noreturn]] void throw_parse_error(const Token& token, const char* message)
{
    throw std::runtime_error(
        "line " + std::to_string(token.line) + ", column "
        + std::to_string(token.column) + ": " + message);
}

const char* visibility_name(Visibility visibility)
{
    return visibility == Visibility::Public ? "public" : "private";
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
            if (argument.name) {
                output += std::string((depth + 1) * 2, ' ')
                    + "NamedArgument(" + *argument.name + ")\n";
                append_dump(*argument.value, depth + 2, output);
            } else {
                append_dump(*argument.value, depth + 1, output);
            }
        }
        return;
    }
    case ExprKind::MemberAccess: {
        const auto& member = static_cast<const MemberAccessExpr&>(expression);
        output += "MemberAccess\n";
        append_dump(*member.object, depth + 1, output);
        output += std::string((depth + 1) * 2, ' ') + member.member + "\n";
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

void append_statement_dump(const Stmt& statement, std::size_t depth, std::string& output)
{
    const std::string indentation(depth * 2, ' ');

    switch (statement.kind) {
    case StmtKind::VariableDeclaration: {
        const auto& declaration = static_cast<const VariableDeclarationStmt&>(statement);
        output += indentation + "VariableDeclaration(" + declaration.name + ")\n";
        output.append((depth + 1) * 2, ' ');
        if (declaration.explicit_type) {
            output += "type: " + *declaration.explicit_type + "\n";
        } else {
            output += "inferred\n";
        }
        append_dump(*declaration.initializer, depth + 1, output);
        return;
    }
    case StmtKind::Assignment: {
        const auto& assignment = static_cast<const AssignmentStmt&>(statement);
        output += indentation + "Assignment(" + assignment.name + ")\n";
        append_dump(*assignment.value, depth + 1, output);
        return;
    }
    case StmtKind::MemberAssignment: {
        const auto& assignment = static_cast<const MemberAssignmentStmt&>(statement);
        output += indentation + "MemberAssignment\n";
        append_dump(*assignment.target, depth + 1, output);
        append_dump(*assignment.value, depth + 1, output);
        return;
    }
    case StmtKind::Expression: {
        const auto& expression = static_cast<const ExpressionStmt&>(statement);
        output += indentation + "ExpressionStatement\n";
        append_dump(*expression.expression, depth + 1, output);
        return;
    }
    case StmtKind::FunctionDeclaration: {
        const auto& function = static_cast<const FunctionDeclarationStmt&>(statement);
        output += indentation + "FunctionDeclaration(" + function.name + ")\n";
        output += std::string((depth + 1) * 2, ' ') + "Parameters\n";
        for (const auto& parameter : function.parameters) {
            output += std::string((depth + 2) * 2, ' ')
                + "Parameter(" + parameter.name + ": " + parameter.type + ")\n";
        }
        if (function.return_type) {
            output += std::string((depth + 1) * 2, ' ')
                + "return type: " + *function.return_type + "\n";
        }
        append_statement_dump(*function.body, depth + 1, output);
        return;
    }
    case StmtKind::Return: {
        const auto& return_statement = static_cast<const ReturnStmt&>(statement);
        output += indentation + "Return\n";
        if (return_statement.value) {
            append_dump(*return_statement.value, depth + 1, output);
        }
        return;
    }
    case StmtKind::Block: {
        const auto& block = static_cast<const BlockStmt&>(statement);
        output += indentation + "Block\n";
        for (const auto& nested_statement : block.statements) {
            append_statement_dump(*nested_statement, depth + 1, output);
        }
        return;
    }
    case StmtKind::If: {
        const auto& if_statement = static_cast<const IfStmt&>(statement);
        output += indentation + "If\n";
        output += std::string((depth + 1) * 2, ' ') + "Condition\n";
        append_dump(*if_statement.condition, depth + 2, output);
        output += std::string((depth + 1) * 2, ' ') + "Then\n";
        append_statement_dump(*if_statement.then_block, depth + 2, output);
        if (if_statement.else_branch) {
            output += std::string((depth + 1) * 2, ' ') + "Else\n";
            append_statement_dump(*if_statement.else_branch, depth + 2, output);
        }
        return;
    }
    case StmtKind::While: {
        const auto& while_statement = static_cast<const WhileStmt&>(statement);
        output += indentation + "While\n";
        output += std::string((depth + 1) * 2, ' ') + "Condition\n";
        append_dump(*while_statement.condition, depth + 2, output);
        append_statement_dump(*while_statement.body, depth + 1, output);
        return;
    }
    case StmtKind::ForIn: {
        const auto& for_statement = static_cast<const ForInStmt&>(statement);
        output += indentation + "ForIn(" + for_statement.variable_name + ")\n";
        output += std::string((depth + 1) * 2, ' ') + "Collection\n";
        append_dump(*for_statement.collection, depth + 2, output);
        append_statement_dump(*for_statement.body, depth + 1, output);
        return;
    }
    case StmtKind::Stop:
        output += indentation + "Stop\n";
        return;
    case StmtKind::Continue:
        output += indentation + "Continue\n";
        return;
    case StmtKind::EnumDeclaration: {
        const auto& declaration = static_cast<const EnumDeclarationStmt&>(statement);
        output += indentation + "EnumDeclaration(" + declaration.name + ")\n";
        for (const auto& variant : declaration.variants) {
            output += std::string((depth + 1) * 2, ' ')
                + "Variant(" + variant.name + ")\n";
            if (variant.payload_type) {
                output += std::string((depth + 2) * 2, ' ')
                    + "PayloadType(" + *variant.payload_type + ")\n";
            }
        }
        return;
    }
    case StmtKind::Handle: {
        const auto& handle = static_cast<const HandleStmt&>(statement);
        output += indentation + "Handle\n";
        output += std::string((depth + 1) * 2, ' ') + "Expression\n";
        append_dump(*handle.expression, depth + 2, output);
        for (const auto& handle_case : handle.cases) {
            output += std::string((depth + 1) * 2, ' ')
                + "Case(" + handle_case.variant_name + ")\n";
            if (handle_case.binding_name) {
                output += std::string((depth + 2) * 2, ' ')
                    + "Binding(" + *handle_case.binding_name + ")\n";
            }
            append_statement_dump(*handle_case.body, depth + 2, output);
        }
        return;
    }
    case StmtKind::StructDeclaration: {
        const auto& declaration = static_cast<const StructDeclarationStmt&>(statement);
        output += indentation + "StructDeclaration(" + declaration.name + ")\n";
        for (const auto& field : declaration.fields) {
            output += std::string((depth + 1) * 2, ' ')
                + "Field(" + field.name + ": " + field.type + ")\n";
            if (field.default_value) {
                output += std::string((depth + 2) * 2, ' ') + "Default\n";
                append_dump(*field.default_value, depth + 3, output);
            }
        }
        return;
    }
    case StmtKind::ClassDeclaration: {
        const auto& declaration = static_cast<const ClassDeclarationStmt&>(statement);
        output += indentation + "ClassDeclaration(" + declaration.name + ")\n";
        for (const auto& member : declaration.members) {
            const std::string member_indentation((depth + 1) * 2, ' ');
            if (member->kind == ClassMemberKind::Field) {
                const auto& field = static_cast<const ClassField&>(*member);
                output += member_indentation + visibility_name(field.visibility)
                    + " Field(" + field.name + ": " + field.type + ")\n";
                if (field.default_value) {
                    output += std::string((depth + 2) * 2, ' ') + "Default\n";
                    append_dump(*field.default_value, depth + 3, output);
                }
                continue;
            }

            const auto& method = static_cast<const MethodDeclaration&>(*member);
            output += member_indentation + visibility_name(method.visibility)
                + " Method(" + method.name + ")\n";
            output += std::string((depth + 2) * 2, ' ') + "Parameters\n";
            for (const auto& parameter : method.parameters) {
                output += std::string((depth + 3) * 2, ' ')
                    + "Parameter(" + parameter.name + ": " + parameter.type + ")\n";
            }
            if (method.return_type) {
                output += std::string((depth + 2) * 2, ' ')
                    + "return type: " + *method.return_type + "\n";
            }
            append_statement_dump(*method.body, depth + 2, output);
        }
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
    auto expression = parse_or();
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
    if (check(TokenType::Function)) {
        return parse_function_declaration();
    }
    if (check(TokenType::Enum)) {
        return parse_enum_declaration();
    }
    if (check(TokenType::Struct)) {
        return parse_struct_declaration();
    }
    if (check(TokenType::Class)) {
        return parse_class_declaration();
    }
    if (check(TokenType::Return)) {
        return parse_return_statement();
    }
    if (check(TokenType::If)) {
        return parse_if_statement();
    }
    if (check(TokenType::While)) {
        return parse_while_statement();
    }
    if (check(TokenType::For)) {
        return parse_for_in_statement();
    }
    if (check(TokenType::Handle)) {
        return parse_handle_statement();
    }
    if (check(TokenType::Stop) || check(TokenType::Continue)) {
        return parse_loop_control_statement();
    }
    if (check(TokenType::Else)) {
        throw_parse_error(peek(), "unexpected 'else' without matching 'if'");
    }
    if (check(TokenType::LeftBrace)) {
        auto block = parse_block_statement();
        require_statement_end();
        return block;
    }
    if (check(TokenType::Identifier)
        && (check_next(TokenType::Declare) || check_next(TokenType::Colon))) {
        return parse_variable_declaration();
    }

    const SourceLocation location{peek().line, peek().column};
    auto expression = parse_or();

    if (match({TokenType::Assign})) {
        const Token assignment = previous();
        if (expression->kind != ExprKind::Identifier
            && expression->kind != ExprKind::MemberAccess) {
            throw_parse_error(
                assignment, "invalid assignment target; expected identifier or member access");
        }

        require_expression("expected assignment value");
        auto value = parse_or();
        require_statement_end();

        if (expression->kind == ExprKind::Identifier) {
            auto name = std::move(static_cast<IdentifierExpr&>(*expression).name);
            return std::make_unique<AssignmentStmt>(
                location, std::move(name), std::move(value));
        }

        auto target = std::unique_ptr<MemberAccessExpr>(
            static_cast<MemberAccessExpr*>(expression.release()));
        return std::make_unique<MemberAssignmentStmt>(
            location, std::move(target), std::move(value));
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
    auto initializer = parse_or();
    require_statement_end();

    return std::make_unique<VariableDeclarationStmt>(
        location, std::move(name.lexeme), std::move(explicit_type), std::move(initializer));
}

std::unique_ptr<Stmt> Parser::parse_function_declaration()
{
    Token function_token = advance();
    const Token& name = consume(TokenType::Identifier, "expected function name");
    const std::string function_name = name.lexeme;
    consume(TokenType::LeftParen, "expected '(' after function name");

    std::vector<Parameter> parameters;
    if (!check(TokenType::RightParen)) {
        do {
            const Token& parameter_name = consume(
                TokenType::Identifier, "expected parameter name");
            Parameter parameter{
                parameter_name.lexeme,
                {},
                SourceLocation{parameter_name.line, parameter_name.column},
            };
            consume(TokenType::Colon, "expected ':' after parameter name");
            const Token& parameter_type = consume(
                TokenType::Identifier, "expected parameter type after ':'");
            parameter.type = parameter_type.lexeme;
            parameters.push_back(std::move(parameter));
        } while (match({TokenType::Comma}));
    }

    consume(TokenType::RightParen, "expected ')' after parameters");

    std::optional<std::string> return_type;
    if (match({TokenType::Arrow})) {
        const Token& type = consume(TokenType::Identifier, "expected return type after '->'");
        return_type = type.lexeme;
    }

    if (!check(TokenType::LeftBrace)) {
        throw_parse_error(peek(), "expected '{' before function body");
    }
    const auto enclosing_loop_depth = loop_depth_;
    loop_depth_ = 0;
    auto body = parse_block_statement();
    loop_depth_ = enclosing_loop_depth;
    require_statement_end();

    return std::make_unique<FunctionDeclarationStmt>(
        SourceLocation{function_token.line, function_token.column},
        function_name,
        std::move(parameters),
        std::move(return_type),
        std::move(body));
}

std::unique_ptr<Stmt> Parser::parse_return_statement()
{
    Token return_token = advance();
    std::unique_ptr<Expr> value;

    if (!at_end()
        && peek().type != TokenType::RightBrace
        && peek().line == return_token.line) {
        value = parse_or();
    }

    require_statement_end();
    return std::make_unique<ReturnStmt>(
        SourceLocation{return_token.line, return_token.column}, std::move(value));
}

std::unique_ptr<Stmt> Parser::parse_if_statement()
{
    Token if_token = advance();
    if (at_end()
        || peek().line != if_token.line
        || peek().type == TokenType::LeftBrace) {
        throw_parse_error(peek(), "expected condition after 'if'");
    }

    auto condition = parse_or();
    if (!check(TokenType::LeftBrace)) {
        throw_parse_error(peek(), "expected '{' after if condition");
    }
    auto then_block = parse_block_statement();

    std::unique_ptr<Stmt> else_branch;
    if (!at_end() && peek().type == TokenType::Else) {
        Token else_token = advance();
        statement_line_ = else_token.line;

        if (check(TokenType::If)) {
            else_branch = parse_if_statement();
        } else if (check(TokenType::LeftBrace)) {
            else_branch = parse_block_statement();
        } else {
            throw_parse_error(peek(), "expected 'if' or '{' after 'else'");
        }
    }

    require_statement_end();
    return std::make_unique<IfStmt>(
        SourceLocation{if_token.line, if_token.column},
        std::move(condition),
        std::move(then_block),
        std::move(else_branch));
}

std::unique_ptr<Stmt> Parser::parse_while_statement()
{
    Token while_token = advance();
    if (at_end()
        || peek().line != while_token.line
        || peek().type == TokenType::LeftBrace) {
        throw_parse_error(peek(), "expected condition after 'while'");
    }

    auto condition = parse_or();
    if (!check(TokenType::LeftBrace)) {
        throw_parse_error(peek(), "expected '{' after while condition");
    }

    ++loop_depth_;
    auto body = parse_block_statement();
    --loop_depth_;
    require_statement_end();

    return std::make_unique<WhileStmt>(
        SourceLocation{while_token.line, while_token.column},
        std::move(condition),
        std::move(body));
}

std::unique_ptr<Stmt> Parser::parse_for_in_statement()
{
    Token for_token = advance();
    const Token& variable = consume(TokenType::Identifier, "expected loop variable after 'for'");
    const std::string variable_name = variable.lexeme;
    consume(TokenType::In, "expected 'in' after loop variable");

    if (at_end()
        || peek().line != for_token.line
        || peek().type == TokenType::LeftBrace) {
        throw_parse_error(peek(), "expected collection expression after 'in'");
    }

    auto collection = parse_or();
    if (!check(TokenType::LeftBrace)) {
        throw_parse_error(peek(), "expected '{' after for collection");
    }

    ++loop_depth_;
    auto body = parse_block_statement();
    --loop_depth_;
    require_statement_end();

    return std::make_unique<ForInStmt>(
        SourceLocation{for_token.line, for_token.column},
        variable_name,
        std::move(collection),
        std::move(body));
}

std::unique_ptr<Stmt> Parser::parse_loop_control_statement()
{
    Token keyword = advance();
    if (loop_depth_ == 0) {
        const char* message = keyword.type == TokenType::Stop
            ? "'stop' is only valid inside a loop"
            : "'continue' is only valid inside a loop";
        throw_parse_error(keyword, message);
    }

    require_statement_end();
    const SourceLocation location{keyword.line, keyword.column};
    if (keyword.type == TokenType::Stop) {
        return std::make_unique<StopStmt>(location);
    }
    return std::make_unique<ContinueStmt>(location);
}

std::unique_ptr<Stmt> Parser::parse_enum_declaration()
{
    Token enum_token = advance();
    const Token& name = consume(TokenType::Identifier, "expected enum name");
    const std::string enum_name = name.lexeme;
    consume(TokenType::LeftBrace, "expected '{' before enum body");

    std::vector<EnumVariant> variants;
    while (!at_end() && peek().type != TokenType::RightBrace) {
        statement_line_ = peek().line;
        const Token& variant_name = consume(
            TokenType::Identifier, "expected enum variant name");
        EnumVariant variant{
            variant_name.lexeme,
            std::nullopt,
            SourceLocation{variant_name.line, variant_name.column},
        };

        if (match({TokenType::LeftParen})) {
            const Token& payload_type = consume(
                TokenType::Identifier, "expected payload type after '('");
            variant.payload_type = payload_type.lexeme;
            consume(TokenType::RightParen, "expected ')' after payload type");
        }

        require_statement_end();
        variants.push_back(std::move(variant));
    }

    if (at_end()) {
        throw_parse_error(peek(), "expected '}' after enum body");
    }
    if (variants.empty()) {
        throw_parse_error(peek(), "enum must declare at least one variant");
    }

    Token right_brace = advance();
    statement_line_ = right_brace.line;
    require_statement_end();
    return std::make_unique<EnumDeclarationStmt>(
        SourceLocation{enum_token.line, enum_token.column},
        enum_name,
        std::move(variants));
}

std::unique_ptr<Stmt> Parser::parse_handle_statement()
{
    Token handle_token = advance();
    if (at_end()
        || peek().line != handle_token.line
        || peek().type == TokenType::LeftBrace) {
        throw_parse_error(peek(), "expected expression after 'handle'");
    }

    auto expression = parse_or();
    if (!check(TokenType::LeftBrace)) {
        throw_parse_error(peek(), "expected '{' after handle expression");
    }
    advance();

    std::vector<HandleCase> cases;
    while (!at_end() && peek().type != TokenType::RightBrace) {
        statement_line_ = peek().line;
        const Token& variant = consume(
            TokenType::Identifier, "expected handle case variant name");
        HandleCase handle_case{
            variant.lexeme,
            std::nullopt,
            nullptr,
            SourceLocation{variant.line, variant.column},
        };

        if (match({TokenType::LeftParen})) {
            const Token& binding = consume(
                TokenType::Identifier, "expected binding name after '('");
            handle_case.binding_name = binding.lexeme;
            consume(TokenType::RightParen, "expected ')' after binding name");
        }

        if (!check(TokenType::LeftBrace)) {
            throw_parse_error(peek(), "expected '{' before handle case body");
        }
        handle_case.body = parse_block_statement();
        require_statement_end();
        cases.push_back(std::move(handle_case));
    }

    if (at_end()) {
        throw_parse_error(peek(), "expected '}' after handle cases");
    }
    if (cases.empty()) {
        throw_parse_error(peek(), "handle must declare at least one case");
    }

    Token right_brace = advance();
    statement_line_ = right_brace.line;
    require_statement_end();
    return std::make_unique<HandleStmt>(
        SourceLocation{handle_token.line, handle_token.column},
        std::move(expression),
        std::move(cases));
}

std::unique_ptr<Stmt> Parser::parse_struct_declaration()
{
    Token struct_token = advance();
    const Token& name = consume(TokenType::Identifier, "expected struct name");
    const std::string struct_name = name.lexeme;
    consume(TokenType::LeftBrace, "expected '{' before struct body");

    std::vector<StructField> fields;
    while (!at_end() && peek().type != TokenType::RightBrace) {
        statement_line_ = peek().line;
        const Token& field_name = consume(
            TokenType::Identifier, "expected struct field name");
        StructField field{
            field_name.lexeme,
            {},
            nullptr,
            SourceLocation{field_name.line, field_name.column},
        };
        consume(TokenType::Colon, "expected ':' after struct field name");
        const Token& field_type = consume(
            TokenType::Identifier, "expected struct field type after ':'");
        field.type = field_type.lexeme;

        if (match({TokenType::Assign})) {
            require_expression("expected default value after '='");
            field.default_value = parse_or();
        }

        require_statement_end();
        fields.push_back(std::move(field));
    }

    if (at_end()) {
        throw_parse_error(peek(), "expected '}' after struct body");
    }

    Token right_brace = advance();
    statement_line_ = right_brace.line;
    require_statement_end();
    return std::make_unique<StructDeclarationStmt>(
        SourceLocation{struct_token.line, struct_token.column},
        struct_name,
        std::move(fields));
}

std::unique_ptr<Stmt> Parser::parse_class_declaration()
{
    Token class_token = advance();
    const Token& name = consume(TokenType::Identifier, "expected class name");
    const std::string class_name = name.lexeme;
    consume(TokenType::LeftBrace, "expected '{' before class body");

    std::vector<std::unique_ptr<ClassMember>> members;
    bool saw_destroy = false;
    while (!at_end() && peek().type != TokenType::RightBrace) {
        statement_line_ = peek().line;
        Visibility visibility = Visibility::Private;
        if (match({TokenType::Public})) {
            visibility = Visibility::Public;
        } else if (match({TokenType::Private})) {
            visibility = Visibility::Private;
        }

        if (check(TokenType::Function)) {
            members.push_back(parse_method_declaration(visibility, saw_destroy));
        } else if (check(TokenType::Identifier)) {
            members.push_back(parse_class_field(visibility));
        } else {
            throw_parse_error(peek(), "expected class field or method");
        }
    }

    if (at_end()) {
        throw_parse_error(peek(), "expected '}' after class body");
    }

    Token right_brace = advance();
    statement_line_ = right_brace.line;
    require_statement_end();
    return std::make_unique<ClassDeclarationStmt>(
        SourceLocation{class_token.line, class_token.column},
        class_name,
        std::move(members));
}

std::unique_ptr<ClassMember> Parser::parse_class_field(Visibility visibility)
{
    Token name = advance();
    consume(TokenType::Colon, "expected ':' after class field name");
    const Token& type = consume(
        TokenType::Identifier, "expected class field type after ':'");
    const std::string type_name = type.lexeme;
    std::unique_ptr<Expr> default_value;

    if (match({TokenType::Assign})) {
        require_expression("expected default value after '='");
        default_value = parse_or();
    }

    require_statement_end();
    return std::make_unique<ClassField>(
        visibility,
        SourceLocation{name.line, name.column},
        std::move(name.lexeme),
        type_name,
        std::move(default_value));
}

std::unique_ptr<ClassMember> Parser::parse_method_declaration(
    Visibility visibility,
    bool& saw_destroy)
{
    Token function_token = advance();
    const Token& name = consume(TokenType::Identifier, "expected method name");
    const std::string method_name = name.lexeme;
    const bool is_destroy = method_name == "destroy";
    if (is_destroy) {
        if (saw_destroy) {
            throw_parse_error(name, "class may declare at most one destroy method");
        }
        saw_destroy = true;
    }

    consume(TokenType::LeftParen, "expected '(' after method name");
    if (is_destroy && !check(TokenType::RightParen)) {
        throw_parse_error(peek(), "destroy method cannot declare parameters");
    }

    std::vector<Parameter> parameters;
    if (!check(TokenType::RightParen)) {
        do {
            const Token& parameter_name = consume(
                TokenType::Identifier, "expected parameter name");
            Parameter parameter{
                parameter_name.lexeme,
                {},
                SourceLocation{parameter_name.line, parameter_name.column},
            };
            consume(TokenType::Colon, "expected ':' after parameter name");
            const Token& parameter_type = consume(
                TokenType::Identifier, "expected parameter type after ':'");
            parameter.type = parameter_type.lexeme;
            parameters.push_back(std::move(parameter));
        } while (match({TokenType::Comma}));
    }
    consume(TokenType::RightParen, "expected ')' after parameters");

    std::optional<std::string> return_type;
    if (match({TokenType::Arrow})) {
        if (is_destroy) {
            throw_parse_error(previous(), "destroy method cannot declare a return type");
        }
        const Token& type = consume(TokenType::Identifier, "expected return type after '->'");
        return_type = type.lexeme;
    }

    if (!check(TokenType::LeftBrace)) {
        throw_parse_error(peek(), "expected '{' before method body");
    }
    const auto enclosing_loop_depth = loop_depth_;
    loop_depth_ = 0;
    auto body = parse_block_statement();
    loop_depth_ = enclosing_loop_depth;
    require_statement_end();

    return std::make_unique<MethodDeclaration>(
        visibility,
        SourceLocation{function_token.line, function_token.column},
        method_name,
        std::move(parameters),
        std::move(return_type),
        std::move(body));
}

std::unique_ptr<BlockStmt> Parser::parse_block_statement()
{
    Token left_brace = advance();
    std::vector<std::unique_ptr<Stmt>> statements;

    while (!at_end() && peek().type != TokenType::RightBrace) {
        statement_line_ = peek().line;
        statements.push_back(parse_statement());
    }

    if (at_end()) {
        throw_parse_error(peek(), "expected '}' after block");
    }

    Token right_brace = advance();
    statement_line_ = right_brace.line;
    return std::make_unique<BlockStmt>(
        SourceLocation{left_brace.line, left_brace.column}, std::move(statements));
}

std::unique_ptr<Expr> Parser::parse_or()
{
    auto expression = parse_and();

    while (match({TokenType::Or})) {
        Token operator_token = previous();
        auto right = parse_and();
        expression = std::make_unique<BinaryExpr>(
            std::move(expression), std::move(operator_token), std::move(right));
    }

    return expression;
}

std::unique_ptr<Expr> Parser::parse_and()
{
    auto expression = parse_equality();

    while (match({TokenType::And})) {
        Token operator_token = previous();
        auto right = parse_equality();
        expression = std::make_unique<BinaryExpr>(
            std::move(expression), std::move(operator_token), std::move(right));
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

    return parse_call();
}

std::unique_ptr<Expr> Parser::parse_call()
{
    auto expression = parse_primary();

    while (true) {
        if (match({TokenType::LeftParen})) {
            expression = finish_call(std::move(expression));
        } else if (match({TokenType::Dot})) {
            const Token& member = consume(
                TokenType::Identifier, "expected member name after '.'");
            expression = std::make_unique<MemberAccessExpr>(
                std::move(expression), member.lexeme);
        } else {
            break;
        }
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
    if (match({TokenType::Identifier, TokenType::Self})) {
        return std::make_unique<IdentifierExpr>(previous().lexeme);
    }
    if (match({TokenType::LeftParen})) {
        auto expression = parse_or();
        consume(TokenType::RightParen, "expected ')' after expression");
        return std::make_unique<GroupingExpr>(std::move(expression));
    }

    throw_parse_error(peek(), "expected expression");
}

std::unique_ptr<Expr> Parser::finish_call(std::unique_ptr<Expr> callee)
{
    const auto enclosing_statement_line = statement_line_;
    statement_line_.reset();

    std::vector<CallArgument> arguments;
    std::unordered_set<std::string> named_arguments;
    bool saw_named_argument = false;
    if (!check(TokenType::RightParen)) {
        do {
            require_expression("expected call argument");
            std::optional<std::string> name;
            if (check(TokenType::Identifier) && check_next(TokenType::Colon)) {
                name = advance().lexeme;
                advance();
                saw_named_argument = true;
                if (!named_arguments.insert(*name).second) {
                    throw_parse_error(previous(), "duplicate named argument");
                }
                if (check(TokenType::Comma) || check(TokenType::RightParen)) {
                    throw_parse_error(peek(), "expected value after named argument");
                }
                require_expression("expected value after named argument");
            } else if (saw_named_argument) {
                throw_parse_error(peek(), "positional argument cannot follow named argument");
            }

            arguments.push_back(CallArgument{std::move(name), parse_or()});
        } while (match({TokenType::Comma}));
    }

    consume(TokenType::RightParen, "expected ')' after call arguments");
    if (enclosing_statement_line) {
        statement_line_ = previous().line;
    } else {
        statement_line_.reset();
    }
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
    if (!at_end()
        && peek().type != TokenType::RightBrace
        && statement_line_
        && peek().line == *statement_line_) {
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
        append_statement_dump(*program.statements[index], 0, output);
        if (index + 1 < program.statements.size()) {
            output += '\n';
        }
    }
    return output;
}

} // namespace toro
