#include "toro/CGenerator.hpp"
#include "toro/Lexer.hpp"
#include "toro/Parser.hpp"
#include "toro/SemanticAnalyzer.hpp"
#include "toro/TypeChecker.hpp"

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

std::string generate(std::string_view source)
{
    auto tokens = toro::Lexer(source).tokenize();
    const auto program = toro::Parser(std::move(tokens)).parse_program();
    toro::SemanticAnalyzer().analyze(program);
    toro::TypeChecker().check(program);
    return toro::CGenerator().generate(program);
}

void expect_contains(
    std::string_view output,
    std::string_view expected,
    std::string_view description)
{
    if (output.find(expected) == std::string_view::npos) {
        throw std::runtime_error(
            "generated C did not contain " + std::string(description)
                + ": " + std::string(expected));
    }
}

void expect_backend_error(std::string_view source, std::string_view expected)
{
    try {
        static_cast<void>(generate(source));
    } catch (const std::runtime_error& error) {
        if (std::string_view(error.what()).find(expected) == std::string_view::npos) {
            throw std::runtime_error(
                "expected backend error containing '" + std::string(expected)
                    + "', got '" + error.what() + "'");
        }
        return;
    }
    throw std::runtime_error("expected C backend failure");
}

void test_primitive_variables_arithmetic_and_assignment()
{
    const auto output = generate(
        "function main() {\n"
        "    count := 10\n"
        "    price := 19.99\n"
        "    active := true\n"
        "    name := \"toro\"\n"
        "    count = count + 2 * 3\n"
        "    print(price)\n"
        "    print(active)\n"
        "    print(name)\n"
        "}\n");

    expect_contains(output, "int64_t toro_var_count = 10;", "integer declaration");
    expect_contains(output, "double toro_var_price = 19.99;", "decimal declaration");
    expect_contains(output, "bool toro_var_active = true;", "boolean declaration");
    expect_contains(output, "const char* toro_var_name = \"toro\";", "string declaration");
    expect_contains(
        output,
        "toro_var_count = (toro_var_count + (2 * 3));",
        "arithmetic reassignment");
    expect_contains(output, "printf(\"%g\\n\", toro_var_price);", "decimal print");
    expect_contains(output, "? \"true\" : \"false\"", "boolean print");
    expect_contains(output, "printf(\"%s\\n\", toro_var_name);", "string print");
}

void test_functions_calls_and_returns()
{
    const auto output = generate(
        "function add(left: int, right: int) -> int {\n"
        "    return left + right\n"
        "}\n"
        "function main() {\n"
        "    result := add(right: 20, left: 10)\n"
        "    print(result)\n"
        "}\n");

    expect_contains(
        output,
        "static int64_t toro_fn_add(int64_t toro_arg_left, int64_t toro_arg_right);",
        "function prototype");
    expect_contains(
        output,
        "return (toro_arg_left + toro_arg_right);",
        "function return");
    expect_contains(
        output,
        "toro_fn_add(10, 20)",
        "deterministically ordered named arguments");
    expect_contains(output, "int main(void)", "C entry point");
}

void test_control_flow_and_boolean_expressions()
{
    const auto output = generate(
        "function main() {\n"
        "    value := 0\n"
        "    running := true\n"
        "    while running and value < 3 {\n"
        "        value = value + 1\n"
        "        if value == 3 or value > 10 {\n"
        "            running = false\n"
        "        } else {\n"
        "            print(value)\n"
        "        }\n"
        "    }\n"
        "}\n");

    expect_contains(
        output,
        "while ((toro_var_running && (toro_var_value < 3)))",
        "while condition");
    expect_contains(
        output,
        "if (((toro_var_value == 3) || (toro_var_value > 10)))",
        "if boolean expression");
    expect_contains(output, "} else {", "else branch");
}

void test_string_equality()
{
    const auto output = generate(
        "function same(left: string, right: string) -> bool {\n"
        "    return left == right\n"
        "}\n");
    expect_contains(output, "strcmp(toro_arg_left, toro_arg_right) == 0", "string equality");
}

void test_unsupported_features()
{
    expect_backend_error(
        "struct Point {\n"
        "    x: int\n"
        "}\n",
        "top-level statements are not supported by the C backend");
    expect_backend_error(
        "function identity<T>(value: T) -> T { return value }\n",
        "generic functions are not supported by the C backend");
    expect_backend_error(
        "function convert(value: int) -> int { return value }\n"
        "function convert(value: string) -> string { return value }\n",
        "function overloads are not supported by the C backend");
}

void test_generated_c_compiles()
{
#ifdef TORO_TEST_C_COMPILER
    const auto output = generate(
        "function square(value: int) -> int {\n"
        "    return value * value\n"
        "}\n"
        "function main() {\n"
        "    value := square(4)\n"
        "    if value >= 16 and value != 0 {\n"
        "        print(value)\n"
        "    } else {\n"
        "        print(0)\n"
        "    }\n"
        "}\n");
    const auto path = std::filesystem::temp_directory_path()
        / "toro_c_generator_syntax_test.c";
    {
        std::ofstream file(path);
        if (!file) {
            throw std::runtime_error("could not create temporary generated C file");
        }
        file << output;
    }
    const std::string command = std::string("\"") + TORO_TEST_C_COMPILER
        + "\" -std=c11 -fsyntax-only \"" + path.string() + "\"";
    const int result = std::system(command.c_str());
    std::filesystem::remove(path);
    if (result != 0) {
        throw std::runtime_error("system C compiler rejected generated output");
    }
#endif
}

} // namespace

int main()
{
    try {
        test_primitive_variables_arithmetic_and_assignment();
        test_functions_calls_and_returns();
        test_control_flow_and_boolean_expressions();
        test_string_equality();
        test_unsupported_features();
        test_generated_c_compiles();
    } catch (const std::exception& error) {
        std::cerr << "C generator test failure: " << error.what() << '\n';
        return 1;
    }

    std::cout << "C generator tests passed\n";
    return 0;
}
