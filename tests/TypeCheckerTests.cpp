#include "toro/Lexer.hpp"
#include "toro/Parser.hpp"
#include "toro/SemanticAnalyzer.hpp"
#include "toro/TypeChecker.hpp"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

void check(std::string_view source)
{
    auto tokens = toro::Lexer(source).tokenize();
    const auto program = toro::Parser(std::move(tokens)).parse_program();
    toro::SemanticAnalyzer().analyze(program);
    toro::TypeChecker().check(program);
}

void expect_valid(std::string_view source)
{
    check(source);
}

void expect_error(std::string_view source, std::string_view expected_message)
{
    try {
        check(source);
    } catch (const std::runtime_error& error) {
        if (std::string_view(error.what()).find(expected_message) == std::string_view::npos) {
            throw std::runtime_error(
                "expected type error containing '" + std::string(expected_message)
                + "', got '" + error.what() + "'");
        }
        return;
    }
    throw std::runtime_error("expected type-checking failure");
}

void test_primitive_inference_and_explicit_types()
{
    expect_valid(
        "integer := 10\n"
        "decimal := 19.99\n"
        "name := \"toro\"\n"
        "active := true\n"
        "print(integer)\n"
        "print(decimal)\n"
        "print(name)\n"
        "print(active)\n");
    expect_valid(
        "integer: int = 10\n"
        "decimal: dec = 19.99\n"
        "name: string = \"toro\"\n"
        "active: bool = false\n");
    expect_error(
        "value: int = \"hello\"\n",
        "cannot assign value of type 'string' to type 'int'");
}

void test_assignment_types()
{
    expect_valid("value := 10\nvalue = 20\n");
    expect_error(
        "value := 10\nvalue = \"hello\"\n",
        "cannot assign value of type 'string' to type 'int'");
    expect_valid(
        "value := 10\n"
        "{\n"
        "    value := \"shadow\"\n"
        "    value = \"updated\"\n"
        "}\n"
        "value = 20\n");
}

void test_arithmetic_and_comparisons()
{
    expect_valid(
        "integer := -10 + 20 * 3 / 2\n"
        "decimal := 1.5 + 2.5\n"
        "less := integer < 100\n"
        "greater := decimal >= 1.0\n"
        "equal := integer == 20\n"
        "different := decimal != 0.0\n");
    expect_error("value := \"hello\" + 1\n", "requires numeric operands");
    expect_error("value := -\"hello\"\n", "unary '-' requires a numeric operand");
    expect_error("value := 1 + 2.0\n", "does not implicitly convert 'int' and 'dec'");
    expect_error("value := true < false\n", "comparison requires numeric operands");
    expect_error("value := 1 == \"1\"\n", "equality operands must have the same type");
}

void test_logical_operators_and_conditions()
{
    expect_valid(
        "active := true\n"
        "ready := false\n"
        "result := active and ready or true\n"
        "if result {\n"
        "    print(result)\n"
        "}\n"
        "while active {\n"
        "    stop\n"
        "}\n");
    expect_error("value := true and 1\n", "requires bool operands");
    expect_error("value := 1 or false\n", "requires bool operands");
    expect_error("if 1 {}\n", "condition must have type 'bool'");
    expect_error("while \"yes\" {}\n", "condition must have type 'bool'");
}

void test_function_arguments()
{
    expect_valid(
        "function main() {\n"
        "    result := add(1, 2)\n"
        "    print(result)\n"
        "}\n"
        "function add(a: int, b: int) -> int {\n"
        "    return a + b\n"
        "}\n");
    expect_error(
        "function add(a: int, b: int) -> int { return a + b }\n"
        "value := add(1)\n",
        "function 'add' expects 2 arguments, got 1");
    expect_error(
        "function echo(value: string) -> string { return value }\n"
        "value := echo(10)\n",
        "argument 1 to 'echo' expects 'string', got 'int'");
    expect_valid(
        "function combine(number: int, text: string) -> string { return text }\n"
        "value := combine(text: \"toro\", number: 10)\n");
    expect_error(
        "function combine(number: int, text: string) -> string { return text }\n"
        "value := combine(text: 10, number: 20)\n",
        "argument 1 to 'combine' expects 'string', got 'int'");
    expect_error(
        "function echo(value: string) -> string { return value }\n"
        "value := echo(other: \"toro\")\n",
        "function 'echo' has no parameter named 'other'");
    expect_error(
        "function nothing() {}\n"
        "value := nothing()\n",
        "expression does not produce a value");
    expect_error(
        "function action() {}\n"
        "function main() {\n"
        "    action := 10\n"
        "    action()\n"
        "}\n",
        "value 'action' of type 'int' is not callable");
}

void test_function_returns()
{
    expect_valid(
        "function value() -> int { return 10 }\n"
        "function action() { return }\n"
        "function unchecked_path() -> bool {}\n");
    expect_error(
        "function value() -> int { return }\n",
        "return requires a value of type 'int'");
    expect_error(
        "function action() { return 10 }\n",
        "a function without a return type cannot return a value");
    expect_error(
        "function value() -> int { return \"wrong\" }\n",
        "cannot assign value of type 'string' to type 'int'");
}

void test_null_restrictions()
{
    expect_valid("print(null)\n");
    expect_error(
        "value := null\n",
        "cannot infer a variable type from null without nullable types");
    expect_error("value: int = null\n", "null requires a nullable type");
    expect_error(
        "value := 10\nvalue = null\n",
        "null requires a nullable type");
    expect_error(
        "value := 10 == null\n",
        "null cannot be compared with non-null type 'int'");
}

void test_existing_language_features_remain_checkable()
{
    expect_valid(
        "function main() {\n"
        "    message := \"Hello, toro!\"\n"
        "    print(message)\n"
        "}\n");
    expect_valid(
        "function main() {\n"
        "    x := 10\n"
        "    if x > 10 {\n"
        "        print(\"greater\")\n"
        "    } else if x == 10 {\n"
        "        print(\"equal\")\n"
        "    }\n"
        "}\n");
    expect_valid(
        "class Box<T> {\n"
        "    value: T\n"
        "    function get() -> T { return self.value }\n"
        "}\n"
        "function identity<T>(value: T) -> T { return value }\n"
        "function main() {\n"
        "    box := Box<int>(value: 10)\n"
        "    value := identity<int>(10)\n"
        "    print(box.value)\n"
        "    print(value)\n"
        "}\n");
}

} // namespace

int main()
{
    try {
        test_primitive_inference_and_explicit_types();
        test_assignment_types();
        test_arithmetic_and_comparisons();
        test_logical_operators_and_conditions();
        test_function_arguments();
        test_function_returns();
        test_null_restrictions();
        test_existing_language_features_remain_checkable();
    } catch (const std::exception& error) {
        std::cerr << "type checker test failure: " << error.what() << '\n';
        return 1;
    }

    std::cout << "type checker tests passed\n";
    return 0;
}
