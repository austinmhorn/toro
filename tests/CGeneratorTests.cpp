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

void test_struct_definition_construction_and_members()
{
    const auto output = generate(
        "struct Player {\n"
        "    name: string\n"
        "    age: int = 28\n"
        "    active: bool\n"
        "    score: dec\n"
        "}\n"
        "function main() {\n"
        "    player := Player(name: \"Austin\")\n"
        "    player.age = 29\n"
        "    print(player.name)\n"
        "}\n");

    expect_contains(
        output,
        "typedef struct toro_struct_Player toro_struct_Player;",
        "struct forward declaration");
    expect_contains(output, "struct toro_struct_Player", "struct definition");
    expect_contains(output, "const char* toro_field_name;", "string field");
    expect_contains(output, "int64_t toro_field_age;", "integer field");
    expect_contains(
        output,
        "(toro_struct_Player){.toro_field_name = \"Austin\", "
        ".toro_field_age = 28, .toro_field_active = false, "
        ".toro_field_score = 0.0}",
        "explicit construction values and defaults");
    expect_contains(
        output, "(toro_var_player).toro_field_age = 29;", "field assignment");
    expect_contains(
        output,
        "printf(\"%s\\n\", (toro_var_player).toro_field_name)",
        "field access");
}

void test_nested_structs_and_value_copy()
{
    const auto output = generate(
        "struct Point {\n"
        "    x: dec\n"
        "    y: dec\n"
        "}\n"
        "struct Sprite {\n"
        "    position: Point\n"
        "    label: string\n"
        "}\n"
        "function main() {\n"
        "    sprite := Sprite(Point(1.0, 2.0), \"hero\")\n"
        "    copy := sprite\n"
        "    copy.position.x = 3.0\n"
        "    print(sprite.position.x)\n"
        "}\n");

    expect_contains(
        output,
        "toro_struct_Point toro_field_position;",
        "nested struct field");
    expect_contains(
        output,
        "toro_struct_Sprite toro_var_copy = toro_var_sprite;",
        "struct value copy");
    expect_contains(
        output,
        "((toro_var_copy).toro_field_position).toro_field_x = 3.0;",
        "chained field assignment");
}

void test_struct_methods()
{
    const auto output = generate(
        "struct Counter {\n"
        "    value: int\n"
        "    function add(amount: int) { self.value = self.value + amount }\n"
        "    function get() -> int { return self.value }\n"
        "}\n"
        "function main() {\n"
        "    counter := Counter()\n"
        "    counter.add(amount: 4)\n"
        "    print(counter.get())\n"
        "}\n");

    expect_contains(
        output,
        "static void toro_method_7_Counter_add(toro_struct_Counter* toro_self, "
        "int64_t toro_arg_amount);",
        "method prototype with receiver");
    expect_contains(
        output,
        "(toro_self)->toro_field_value = "
        "((toro_self)->toro_field_value + toro_arg_amount);",
        "self mutation");
    expect_contains(
        output,
        "toro_method_7_Counter_add(&(toro_var_counter), 4);",
        "method call receiver");
    expect_contains(
        output,
        "toro_method_7_Counter_get(&(toro_var_counter))",
        "method result call");
}

void test_enum_representation_construction_and_handle()
{
    const auto output = generate(
        "enum Value {\n"
        "    integer(int)\n"
        "    decimal(dec)\n"
        "    flag(bool)\n"
        "    text(string)\n"
        "    empty\n"
        "}\n"
        "function show(value: Value) {\n"
        "    handle value {\n"
        "        integer(item) { print(item) }\n"
        "        decimal(item) { print(item) }\n"
        "        flag(item) { print(item) }\n"
        "        text(item) { print(item) }\n"
        "        empty { print(\"empty\") }\n"
        "    }\n"
        "}\n"
        "function main() {\n"
        "    value := Value::text(\"toro\")\n"
        "    value = Value::empty\n"
        "    show(value)\n"
        "}\n");

    expect_contains(
        output,
        "typedef enum toro_enum_5_Value_tag",
        "enum tag declaration");
    expect_contains(
        output,
        "struct toro_enum_5_Value",
        "tagged enum structure");
    expect_contains(output, "int64_t toro_variant_integer;", "integer payload");
    expect_contains(output, "double toro_variant_decimal;", "decimal payload");
    expect_contains(output, "bool toro_variant_flag;", "boolean payload");
    expect_contains(output, "const char* toro_variant_text;", "string payload");
    expect_contains(
        output,
        "(toro_enum_5_Value){.toro_tag = toro_enum_5_Value_tag_text, "
        ".toro_payload.toro_variant_text = \"toro\"}",
        "payload variant construction");
    expect_contains(
        output,
        "toro_var_value = (toro_enum_5_Value){.toro_tag = "
        "toro_enum_5_Value_tag_empty};",
        "payload-free variant assignment");
    expect_contains(
        output,
        "switch (toro_handle_value_0.toro_tag)",
        "handle switch");
    expect_contains(
        output,
        "const char* toro_var_item = "
        "toro_handle_value_0.toro_payload.toro_variant_text;",
        "scoped payload binding");
}

void test_enum_function_return_and_nested_payload_types()
{
    const auto output = generate(
        "struct Point { x: int }\n"
        "enum Inner { number(int) }\n"
        "enum Outer {\n"
        "    nested(Inner)\n"
        "    point(Point)\n"
        "    done\n"
        "}\n"
        "function make() -> Outer {\n"
        "    return Outer::nested(Inner::number(7))\n"
        "}\n"
        "function main() {\n"
        "    first := make()\n"
        "    copy := first\n"
        "    copy = Outer::point(Point(3))\n"
        "}\n");

    expect_contains(
        output,
        "toro_enum_5_Inner toro_variant_nested;",
        "enum payload type");
    expect_contains(
        output,
        "toro_struct_Point toro_variant_point;",
        "struct payload type");
    expect_contains(
        output,
        "static toro_enum_5_Outer toro_fn_make(void);",
        "enum function return");
    expect_contains(
        output,
        "toro_enum_5_Outer toro_var_copy = toro_var_first;",
        "enum value copy");
}

void test_result_construction_handle_and_propagation()
{
    const auto output = generate(
        "function parse(valid: bool) -> Result<int, string> {\n"
        "    if valid { return ok(7) }\n"
        "    return error(\"bad\")\n"
        "}\n"
        "function forward(valid: bool) -> Result<int, string> {\n"
        "    value := parse(valid)?\n"
        "    return ok(value)\n"
        "}\n"
        "function show(result: Result<int, string>) {\n"
        "    handle result {\n"
        "        ok(value) { print(value) }\n"
        "        error(problem) { print(problem) }\n"
        "    }\n"
        "}\n"
        "function main() {\n"
        "    success: Result<int, string> = forward(true)\n"
        "    copy := success\n"
        "    show(copy)\n"
        "    show(forward(false))\n"
        "}\n");

    expect_contains(
        output,
        "typedef struct toro_result_r1_i_1_s toro_result_r1_i_1_s;",
        "concrete Result declaration");
    expect_contains(output, "toro_result_r1_i_1_s_tag_ok", "Result ok tag");
    expect_contains(output, "int64_t toro_ok;", "Result ok payload");
    expect_contains(output, "const char* toro_error;", "Result error payload");
    expect_contains(
        output,
        ".toro_payload.toro_ok = 7",
        "ok construction");
    expect_contains(
        output,
        ".toro_payload.toro_error = \"bad\"",
        "error construction");
    expect_contains(
        output,
        "if (toro_result_value_0.toro_tag == "
        "toro_result_r1_i_1_s_tag_error)",
        "propagation error check");
    expect_contains(
        output,
        "switch (toro_handle_value_1.toro_tag)",
        "Result handle switch");
    expect_contains(
        output,
        "toro_result_r1_i_1_s toro_var_copy = toro_var_success;",
        "Result value copy");
}

void test_distinct_result_instances()
{
    const auto output = generate(
        "function first() -> Result<int, string> { return ok(1) }\n"
        "function second() -> Result<string, int> { return ok(\"two\") }\n");
    expect_contains(output, "toro_result_r1_i_1_s", "first Result instance");
    expect_contains(output, "toro_result_r1_s_1_i", "second Result instance");
}

void test_unsupported_features()
{
    expect_backend_error(
        "class Point {\n"
        "    public x: int\n"
        "}\n",
        "top-level statements are not supported by the C backend");
    expect_backend_error(
        "function identity<T>(value: T) -> T { return value }\n",
        "generic functions are not supported by the C backend");
    expect_backend_error(
        "function convert(value: int) -> int { return value }\n"
        "function convert(value: string) -> string { return value }\n",
        "function overloads are not supported by the C backend");
    expect_backend_error(
        "struct Box<T> { value: T }\n",
        "generic structs are not supported by the C backend");
    expect_backend_error(
        "struct MaybeOwner { owner: string? }\n",
        "nullable type 'string?' is not supported by the C backend");
    expect_backend_error(
        "struct Named {\n"
        "    name: string\n"
        "    overload as string { return self.name }\n"
        "}\n",
        "conversion overloads are not supported by the C backend");
    expect_backend_error(
        "struct Counter {\n"
        "    value: int\n"
        "    function increment() { self.value = self.value + 1 }\n"
        "}\n"
        "function main() { Counter().increment() }\n",
        "receiver must be an addressable value");
    expect_backend_error(
        "class Resource {}\n"
        "enum Event { resource(Resource) }\n",
        "type 'Resource' is not supported by the C backend");
    expect_backend_error(
        "class Resource {}\n"
        "function consume(result: Result<Resource, string>) {}\n",
        "type 'Resource' is not supported by the C backend");
    expect_backend_error(
        "function consume(result: Result<int?, string>) {}\n",
        "nullable type 'int?' is not supported by the C backend");
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
        test_struct_definition_construction_and_members();
        test_nested_structs_and_value_copy();
        test_struct_methods();
        test_enum_representation_construction_and_handle();
        test_enum_function_return_and_nested_payload_types();
        test_result_construction_handle_and_propagation();
        test_distinct_result_instances();
        test_unsupported_features();
        test_generated_c_compiles();
    } catch (const std::exception& error) {
        std::cerr << "C generator test failure: " << error.what() << '\n';
        return 1;
    }

    std::cout << "C generator tests passed\n";
    return 0;
}
