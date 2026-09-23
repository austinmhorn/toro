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

void expect_dump(std::string_view source, std::string_view expected)
{
    const auto expression = parse(source);
    expect(toro::dump_expression(*expression) == expected, "unexpected expression tree");
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
        test_left_associativity();
        test_remaining_binary_operators();
        test_failures();
    } catch (const std::exception& error) {
        std::cerr << "parser test failure: " << error.what() << '\n';
        return 1;
    }

    std::cout << "parser tests passed\n";
    return 0;
}
