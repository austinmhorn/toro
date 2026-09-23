#include "toro/AST.hpp"
#include "toro/Lexer.hpp"
#include "toro/Parser.hpp"

#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

void expect(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

std::unique_ptr<toro::Expr> parse(std::string_view source)
{
    auto tokens = toro::Lexer(source).tokenize();
    return toro::Parser(std::move(tokens)).parse_expression();
}

toro::Program parse_program(std::string_view source)
{
    auto tokens = toro::Lexer(source).tokenize();
    return toro::Parser(std::move(tokens)).parse_program();
}

void expect_dump(std::string_view source, std::string_view expected)
{
    const auto expression = parse(source);
    expect(toro::dump_expression(*expression) == expected, "unexpected expression tree");
}

void expect_program_dump(std::string_view source, std::string_view expected)
{
    const auto program = parse_program(source);
    expect(toro::dump_program(program) == expected, "unexpected statement tree");
}

void test_literals_and_identifier()
{
    expect_dump("42", "Integer(42)\n");
    expect_dump("19.99", "Decimal(19.99)\n");
    expect_dump("\"hello, toro!\"", "String(hello, toro!)\n");
    expect_dump("true", "Bool(true)\n");
    expect_dump("false", "Bool(false)\n");
    expect_dump("null", "Null\n");
    expect_dump("answer", "Identifier(answer)\n");
}

void test_unary_minus()
{
    expect_dump("-10", "Unary(-)\n  Integer(10)\n");
}

void test_multiplication_before_addition()
{
    expect_dump(
        "10 + 20 * 3",
        "Binary(+)\n"
        "  Integer(10)\n"
        "  Binary(*)\n"
        "    Integer(20)\n"
        "    Integer(3)\n");
}

void test_parentheses_override_precedence()
{
    expect_dump(
        "(10 + 20) * 3",
        "Binary(*)\n"
        "  Grouping\n"
        "    Binary(+)\n"
        "      Integer(10)\n"
        "      Integer(20)\n"
        "  Integer(3)\n");
}

void test_comparison_precedence()
{
    expect_dump(
        "1 + 2 < 3 * 4",
        "Binary(<)\n"
        "  Binary(+)\n"
        "    Integer(1)\n"
        "    Integer(2)\n"
        "  Binary(*)\n"
        "    Integer(3)\n"
        "    Integer(4)\n");
}

void test_equality_precedence()
{
    expect_dump(
        "a >= 10 == true",
        "Binary(==)\n"
        "  Binary(>=)\n"
        "    Identifier(a)\n"
        "    Integer(10)\n"
        "  Bool(true)\n");
}

void test_logical_operators()
{
    expect_dump(
        "active and logged_in",
        "Binary(and)\n"
        "  Identifier(active)\n"
        "  Identifier(logged_in)\n");
    expect_dump(
        "admin or owner",
        "Binary(or)\n"
        "  Identifier(admin)\n"
        "  Identifier(owner)\n");
}

void test_logical_precedence()
{
    expect_dump(
        "a == 1 or b == 2 and c == 3",
        "Binary(or)\n"
        "  Binary(==)\n"
        "    Identifier(a)\n"
        "    Integer(1)\n"
        "  Binary(and)\n"
        "    Binary(==)\n"
        "      Identifier(b)\n"
        "      Integer(2)\n"
        "    Binary(==)\n"
        "      Identifier(c)\n"
        "      Integer(3)\n");
    expect_dump(
        "a > 1 and b <= 2",
        "Binary(and)\n"
        "  Binary(>)\n"
        "    Identifier(a)\n"
        "    Integer(1)\n"
        "  Binary(<=)\n"
        "    Identifier(b)\n"
        "    Integer(2)\n");
}

void test_left_associativity()
{
    expect_dump(
        "10 - 5 - 2",
        "Binary(-)\n"
        "  Binary(-)\n"
        "    Integer(10)\n"
        "    Integer(5)\n"
        "  Integer(2)\n");
}

void test_remaining_binary_operators()
{
    expect_dump("8 / 2", "Binary(/)\n  Integer(8)\n  Integer(2)\n");
    expect_dump("1 != 2", "Binary(!=)\n  Integer(1)\n  Integer(2)\n");
    expect_dump("1 <= 2", "Binary(<=)\n  Integer(1)\n  Integer(2)\n");
    expect_dump("2 > 1", "Binary(>)\n  Integer(2)\n  Integer(1)\n");
}

void test_variable_declarations()
{
    expect_program_dump(
        "x := 10",
        "VariableDeclaration(x)\n"
        "  inferred\n"
        "  Integer(10)\n");
    expect_program_dump(
        "price: dec = 19.99",
        "VariableDeclaration(price)\n"
        "  type: dec\n"
        "  Decimal(19.99)\n");

    const auto program = parse_program("\n  value := 1");
    expect(program.statements.size() == 1, "expected one declaration");
    const auto& declaration = static_cast<const toro::VariableDeclarationStmt&>(
        *program.statements.front());
    expect(declaration.location.line == 2, "declaration line was not retained");
    expect(declaration.location.column == 3, "declaration column was not retained");
}

void test_declaration_and_assignment_are_distinct()
{
    const auto program = parse_program("x := 10\nx = 20");
    expect(program.statements.size() == 2, "expected two statements");
    expect(program.statements[0]->kind == toro::StmtKind::VariableDeclaration,
        "declaration was not represented as a declaration");
    expect(program.statements[1]->kind == toro::StmtKind::Assignment,
        "assignment was not represented as an assignment");
}

void test_assignment_expression()
{
    expect_program_dump(
        "x = x + 1",
        "Assignment(x)\n"
        "  Binary(+)\n"
        "    Identifier(x)\n"
        "    Integer(1)\n");
}

void test_multiple_statements_and_blank_lines()
{
    expect_program_dump(
        "x := 10\n\nprice: dec = 19.99\n\nx = x + 5\n\nprint(x)\n",
        "VariableDeclaration(x)\n"
        "  inferred\n"
        "  Integer(10)\n"
        "\n"
        "VariableDeclaration(price)\n"
        "  type: dec\n"
        "  Decimal(19.99)\n"
        "\n"
        "Assignment(x)\n"
        "  Binary(+)\n"
        "    Identifier(x)\n"
        "    Integer(5)\n"
        "\n"
        "ExpressionStatement\n"
        "  Call\n"
        "    Identifier(print)\n"
        "    Identifier(x)\n");
}

void test_call_expression_statement()
{
    expect_program_dump(
        "add(10, 20)",
        "ExpressionStatement\n"
        "  Call\n"
        "    Identifier(add)\n"
        "    Integer(10)\n"
        "    Integer(20)\n");
}

void test_empty_function()
{
    expect_program_dump(
        "function main() {\n}\n",
        "FunctionDeclaration(main)\n"
        "  Parameters\n"
        "  Block\n");
}

void test_function_parameters_and_return_type()
{
    expect_program_dump(
        "function add(a: int, b: int) -> int {\n"
        "    return a + b\n"
        "}\n",
        "FunctionDeclaration(add)\n"
        "  Parameters\n"
        "    Parameter(a: int)\n"
        "    Parameter(b: int)\n"
        "  return type: int\n"
        "  Block\n"
        "    Return\n"
        "      Binary(+)\n"
        "        Identifier(a)\n"
        "        Identifier(b)\n");

    const auto program = parse_program("function one(value: dec) {}\n");
    const auto& function = static_cast<const toro::FunctionDeclarationStmt&>(
        *program.statements.front());
    expect(function.location.line == 1 && function.location.column == 1,
        "function location was not retained");
    expect(function.parameters.front().location.column == 14,
        "parameter location was not retained");
}

void test_return_without_value()
{
    expect_program_dump(
        "function finish() {\n"
        "    return\n"
        "}\n",
        "FunctionDeclaration(finish)\n"
        "  Parameters\n"
        "  Block\n"
        "    Return\n");
}

void test_nested_function_statements()
{
    expect_program_dump(
        "function main() {\n"
        "    result := add(10, 20)\n"
        "    print(result)\n"
        "}\n",
        "FunctionDeclaration(main)\n"
        "  Parameters\n"
        "  Block\n"
        "    VariableDeclaration(result)\n"
        "      inferred\n"
        "      Call\n"
        "        Identifier(add)\n"
        "        Integer(10)\n"
        "        Integer(20)\n"
        "    ExpressionStatement\n"
        "      Call\n"
        "        Identifier(print)\n"
        "        Identifier(result)\n");
}

void test_multiple_functions()
{
    const auto program = parse_program(
        "function first() {\n}\n\n"
        "function second(value: int) -> int {\n"
        "    return value\n"
        "}\n");
    expect(program.statements.size() == 2, "expected two function declarations");
    expect(program.statements[0]->kind == toro::StmtKind::FunctionDeclaration,
        "first statement was not a function");
    expect(program.statements[1]->kind == toro::StmtKind::FunctionDeclaration,
        "second statement was not a function");
}

void test_standalone_block()
{
    expect_program_dump(
        "{\n"
        "    x := 10\n"
        "    print(x)\n"
        "}\n",
        "Block\n"
        "  VariableDeclaration(x)\n"
        "    inferred\n"
        "    Integer(10)\n"
        "  ExpressionStatement\n"
        "    Call\n"
        "      Identifier(print)\n"
        "      Identifier(x)\n");
}

void test_basic_if()
{
    expect_program_dump(
        "if x > 5 {\n"
        "    print(\"large\")\n"
        "}\n",
        "If\n"
        "  Condition\n"
        "    Binary(>)\n"
        "      Identifier(x)\n"
        "      Integer(5)\n"
        "  Then\n"
        "    Block\n"
        "      ExpressionStatement\n"
        "        Call\n"
        "          Identifier(print)\n"
        "          String(large)\n");

    const auto program = parse_program("\n  if true {\n  }\n");
    const auto& if_statement = static_cast<const toro::IfStmt&>(
        *program.statements.front());
    expect(if_statement.location.line == 2 && if_statement.location.column == 3,
        "if source location was not retained");
}

void test_if_else()
{
    expect_program_dump(
        "if x > 5 {\n"
        "    print(\"large\")\n"
        "} else {\n"
        "    print(\"small\")\n"
        "}\n",
        "If\n"
        "  Condition\n"
        "    Binary(>)\n"
        "      Identifier(x)\n"
        "      Integer(5)\n"
        "  Then\n"
        "    Block\n"
        "      ExpressionStatement\n"
        "        Call\n"
        "          Identifier(print)\n"
        "          String(large)\n"
        "  Else\n"
        "    Block\n"
        "      ExpressionStatement\n"
        "        Call\n"
        "          Identifier(print)\n"
        "          String(small)\n");
}

void test_else_if_chains()
{
    const auto program = parse_program(
        "if first {\n}\n"
        "else if second {\n}\n"
        "else if third {\n}\n"
        "else {\n}\n");
    const auto& first = static_cast<const toro::IfStmt&>(*program.statements.front());
    expect(first.else_branch->kind == toro::StmtKind::If,
        "first else-if branch was not an if statement");
    const auto& second = static_cast<const toro::IfStmt&>(*first.else_branch);
    expect(second.else_branch->kind == toro::StmtKind::If,
        "second else-if branch was not an if statement");
    const auto& third = static_cast<const toro::IfStmt&>(*second.else_branch);
    expect(third.else_branch->kind == toro::StmtKind::Block,
        "final else branch was not a block");
}

void test_nested_if_and_return()
{
    const auto program = parse_program(
        "function check(x: int, y: int) -> int {\n"
        "    if x > 0 {\n"
        "        if y > 0 {\n"
        "            return x\n"
        "        }\n"
        "    }\n"
        "    return 0\n"
        "}\n");
    const auto& function = static_cast<const toro::FunctionDeclarationStmt&>(
        *program.statements.front());
    const auto& outer_if = static_cast<const toro::IfStmt&>(*function.body->statements.front());
    expect(outer_if.then_block->statements.front()->kind == toro::StmtKind::If,
        "nested if was not retained in the outer block");
    const auto& inner_if = static_cast<const toro::IfStmt&>(
        *outer_if.then_block->statements.front());
    expect(inner_if.then_block->statements.front()->kind == toro::StmtKind::Return,
        "return was not retained inside the nested if");
}

void test_declarations_and_assignments_in_branches()
{
    const auto program = parse_program(
        "if ready {\n"
        "    value := 1\n"
        "} else {\n"
        "    value = 2\n"
        "}\n");
    const auto& if_statement = static_cast<const toro::IfStmt&>(
        *program.statements.front());
    expect(if_statement.then_block->statements.front()->kind
            == toro::StmtKind::VariableDeclaration,
        "declaration was not retained in then branch");
    const auto& else_block = static_cast<const toro::BlockStmt&>(
        *if_statement.else_branch);
    expect(else_block.statements.front()->kind == toro::StmtKind::Assignment,
        "assignment was not retained in else branch");
}

void test_logical_operators_in_if_condition()
{
    const auto program = parse_program(
        "if active and logged_in or admin {\n"
        "    print(\"allowed\")\n"
        "}\n");
    const auto& if_statement = static_cast<const toro::IfStmt&>(
        *program.statements.front());
    expect(
        toro::dump_expression(*if_statement.condition)
            == "Binary(or)\n"
               "  Binary(and)\n"
               "    Identifier(active)\n"
               "    Identifier(logged_in)\n"
               "  Identifier(admin)\n",
        "logical operators parsed incorrectly in if condition");
}

void expect_parse_error(std::string_view source, std::string_view expected_message)
{
    try {
        static_cast<void>(parse(source));
    } catch (const std::runtime_error& error) {
        expect(std::string_view(error.what()).find(expected_message) != std::string_view::npos,
            "parse error message was not descriptive");
        return;
    }
    throw std::runtime_error("expected parser failure");
}

void test_failures()
{
    expect_parse_error("10 +", "line 1, column 5: expected expression");
    expect_parse_error("(10 + 20", "line 1, column 9: expected ')' after expression");
    expect_parse_error("* 10", "line 1, column 1: expected expression");
    expect_parse_error("10 20", "line 1, column 4: unexpected token after expression");
}

void expect_program_error(std::string_view source, std::string_view expected_message)
{
    try {
        static_cast<void>(parse_program(source));
    } catch (const std::runtime_error& error) {
        expect(std::string_view(error.what()).find(expected_message) != std::string_view::npos,
            "statement error message was not descriptive");
        return;
    }
    throw std::runtime_error("expected statement parser failure");
}

void test_statement_failures()
{
    expect_program_error("x: = 10", "line 1, column 4: expected type name after ':'");
    expect_program_error("10 = x", "line 1, column 4: invalid assignment target");
    expect_program_error("x :=", "line 1, column 5: expected variable initializer");
    expect_program_error("x: int", "line 1, column 7: expected '=' and initializer");
    expect_program_error("x := 10\n* 2", "line 2, column 1: expected expression");
    expect_program_error(":= 10", "line 1, column 1: expected expression");
}

void test_function_failures()
{
    expect_program_error(
        "function bad(a: int,) {}",
        "expected parameter name");
    expect_program_error(
        "function bad(a) {}",
        "expected ':' after parameter name");
    expect_program_error(
        "function bad(a:) {}",
        "expected parameter type after ':'");
    expect_program_error(
        "function bad() {\n    return\n",
        "expected '}' after block");
    expect_program_error(
        "function bad() {\n    return + 1\n}\n",
        "expected expression");
}

void test_if_failures()
{
    expect_program_error(
        "if {\n}\n",
        "expected condition after 'if'");
    expect_program_error(
        "if x > 5\n    print(x)\n",
        "expected '{' after if condition");
    expect_program_error(
        "if x > 5 {\n    print(x)\n",
        "expected '}' after block");
    expect_program_error(
        "else {\n}\n",
        "unexpected 'else' without matching 'if'");
    expect_program_error(
        "if first {\n}\nelse if second\n    print(second)\n",
        "expected '{' after if condition");
}

} // namespace

int main()
{
    try {
        test_literals_and_identifier();
        test_unary_minus();
        test_multiplication_before_addition();
        test_parentheses_override_precedence();
        test_comparison_precedence();
        test_equality_precedence();
        test_logical_operators();
        test_logical_precedence();
        test_left_associativity();
        test_remaining_binary_operators();
        test_variable_declarations();
        test_declaration_and_assignment_are_distinct();
        test_assignment_expression();
        test_multiple_statements_and_blank_lines();
        test_call_expression_statement();
        test_empty_function();
        test_function_parameters_and_return_type();
        test_return_without_value();
        test_nested_function_statements();
        test_multiple_functions();
        test_standalone_block();
        test_basic_if();
        test_if_else();
        test_else_if_chains();
        test_nested_if_and_return();
        test_declarations_and_assignments_in_branches();
        test_logical_operators_in_if_condition();
        test_failures();
        test_statement_failures();
        test_function_failures();
        test_if_failures();
    } catch (const std::exception& error) {
        std::cerr << "parser test failure: " << error.what() << '\n';
        return 1;
    }

    std::cout << "parser tests passed\n";
    return 0;
}
