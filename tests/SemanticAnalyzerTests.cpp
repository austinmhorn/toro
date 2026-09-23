#include "toro/Lexer.hpp"
#include "toro/Parser.hpp"
#include "toro/SemanticAnalyzer.hpp"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

void analyze(std::string_view source)
{
    auto tokens = toro::Lexer(source).tokenize();
    const auto program = toro::Parser(std::move(tokens)).parse_program();
    toro::SemanticAnalyzer().analyze(program);
}

void expect_valid(std::string_view source)
{
    analyze(source);
}

void expect_error(std::string_view source, std::string_view expected_message)
{
    try {
        analyze(source);
    } catch (const std::runtime_error& error) {
        if (std::string_view(error.what()).find(expected_message) == std::string_view::npos) {
            throw std::runtime_error(
                "expected semantic error containing '" + std::string(expected_message)
                + "', got '" + error.what() + "'");
        }
        return;
    }
    throw std::runtime_error("expected semantic analysis failure");
}

void test_variable_lookup_and_duplicates()
{
    expect_valid("x := 10\nprint(x)\n");
    expect_error(
        "print(x)\n",
        "line 1, column 1: semantic error: unknown identifier 'x'");
    expect_error("x := 10\nx := 20\n", "duplicate declaration 'x' in this scope");
    expect_error("x = 10\n", "unknown identifier 'x'");
}

void test_nested_shadowing_and_block_scope()
{
    expect_valid(
        "x := 10\n"
        "{\n"
        "    x := 20\n"
        "    print(x)\n"
        "}\n"
        "print(x)\n");
    expect_error(
        "{\n"
        "    hidden := 10\n"
        "}\n"
        "print(hidden)\n",
        "unknown identifier 'hidden'");
    expect_error(
        "if true {\n"
        "    branch_value := 1\n"
        "}\n"
        "print(branch_value)\n",
        "unknown identifier 'branch_value'");
}

void test_function_parameters_and_lookup()
{
    expect_valid(
        "function main() {\n"
        "    value := add(1, 2)\n"
        "    print(value)\n"
        "}\n"
        "\n"
        "function add(a: int, b: int) -> int {\n"
        "    return a + b\n"
        "}\n");
    expect_error(
        "function main() {\n"
        "    missing()\n"
        "}\n",
        "unknown identifier 'missing'");
    expect_error(
        "function duplicate(value: int, value: int) {}\n",
        "duplicate declaration 'value' in this scope");
    expect_valid(
        "function shadow(value: int) {\n"
        "    {\n"
        "        value := 2\n"
        "        print(value)\n"
        "    }\n"
        "    print(value)\n"
        "}\n");
}

void test_loop_variable_scope()
{
    expect_valid(
        "items := 1\n"
        "for item in items {\n"
        "    print(item)\n"
        "}\n");
    expect_error(
        "items := 1\n"
        "for item in items {\n"
        "    print(item)\n"
        "}\n"
        "print(item)\n",
        "unknown identifier 'item'");
}

void test_handle_binding_scope()
{
    expect_valid(
        "message := 1\n"
        "handle message {\n"
        "    text(value) {\n"
        "        print(value)\n"
        "    }\n"
        "    quit {}\n"
        "}\n");
    expect_error(
        "message := 1\n"
        "handle message {\n"
        "    text(value) {\n"
        "        print(value)\n"
        "    }\n"
        "}\n"
        "print(value)\n",
        "unknown identifier 'value'");
    expect_error(
        "message := 1\n"
        "handle message {\n"
        "    text(value) {\n"
        "        value := 2\n"
        "    }\n"
        "}\n",
        "duplicate declaration 'value' in this scope");
}

void test_self_context()
{
    expect_valid(
        "class Player {\n"
        "    value: int\n"
        "    function set(value: int) {\n"
        "        self.value = value\n"
        "    }\n"
        "}\n");
    expect_error("print(self)\n", "'self' is only valid inside class or struct methods");
    expect_error(
        "class Player {\n"
        "    function outer() {\n"
        "        function nested() {\n"
        "            print(self)\n"
        "        }\n"
        "    }\n"
        "}\n",
        "'self' is only valid inside class or struct methods");
}

void test_duplicate_declared_symbols()
{
    expect_error(
        "function duplicate() {}\n"
        "function duplicate() {}\n",
        "duplicate declaration 'duplicate' in this scope");
    expect_error(
        "struct Item {}\n"
        "class Item {}\n",
        "duplicate declaration 'Item' in this scope");
    expect_error(
        "interface Value {}\n"
        "enum Value { none }\n",
        "duplicate declaration 'Value' in this scope");
}

void test_declared_types_as_constructors()
{
    expect_valid(
        "struct Pair<A, B> {\n"
        "    first: A\n"
        "    second: B\n"
        "}\n"
        "pair := Pair<int, string>(first: 1, second: \"one\")\n");
}

} // namespace

int main()
{
    try {
        test_variable_lookup_and_duplicates();
        test_nested_shadowing_and_block_scope();
        test_function_parameters_and_lookup();
        test_loop_variable_scope();
        test_handle_binding_scope();
        test_self_context();
        test_duplicate_declared_symbols();
        test_declared_types_as_constructors();
    } catch (const std::exception& error) {
        std::cerr << "semantic test failure: " << error.what() << '\n';
        return 1;
    }

    std::cout << "semantic tests passed\n";
    return 0;
}
