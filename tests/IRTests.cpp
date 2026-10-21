#include "toro/CGenerator.hpp"
#include "toro/ArcOptimizer.hpp"
#include "toro/BorrowInferenceAnalyzer.hpp"
#include "toro/EffectAnalyzer.hpp"
#include "toro/IR.hpp"
#include "toro/IRLowerer.hpp"
#include "toro/Lexer.hpp"
#include "toro/OwnershipSummaryAnalyzer.hpp"
#include "toro/ModuleLoader.hpp"
#include "toro/Parser.hpp"
#include "toro/SemanticAnalyzer.hpp"
#include "toro/TypeChecker.hpp"

#include <exception>
#include <filesystem>
#include <iostream>
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

void expect_contains(
    std::string_view output,
    std::string_view expected,
    std::string_view description)
{
    expect(output.find(expected) != std::string_view::npos,
        std::string("IR dump did not contain ") + std::string(description)
            + ": " + std::string(expected));
}

toro::Program parse_and_check(std::string_view source)
{
    auto tokens = toro::Lexer(source).tokenize();
    auto program = toro::Parser(std::move(tokens)).parse_program();
    toro::SemanticAnalyzer().analyze(program);
    toro::TypeChecker().check(program);
    return program;
}

toro::Program load_and_check(
    const std::filesystem::path& entry,
    const std::filesystem::path& source_root = {})
{
    auto program = source_root.empty()
        ? toro::ModuleLoader().load(entry)
        : toro::ModuleLoader().load(entry, source_root);
    toro::SemanticAnalyzer().analyze(program);
    toro::TypeChecker().check(program);
    return program;
}

toro::IRProgram lower_detached(std::string_view source)
{
    auto program = parse_and_check(source);
    auto ir = toro::IRLowerer().lower(program);
    toro::EffectAnalyzer().analyze(ir);
    toro::OwnershipSummaryAnalyzer().analyze(ir);
    toro::ArcMaterializer().materialize(ir);
    toro::OwnershipTransferAnalyzer().analyze(ir);
    toro::BorrowInferenceAnalyzer().analyze(ir);
    toro::ArcOptimizer().optimize(ir);
    return ir;
}

void test_typed_operations_and_control_flow()
{
    auto program = parse_and_check(
        "function compute(value: int) -> int {\n"
        "    result := value + 2\n"
        "    while result < 5 {\n"
        "        result = result + 1\n"
        "    }\n"
        "    if result == 5 {\n"
        "        return result\n"
        "    } else {\n"
        "        return 0\n"
        "    }\n"
        "}\n");
    const auto ir = toro::IRLowerer().lower(program);
    const auto dump = toro::dump_ir(ir);

    expect_contains(dump, "toro-ir 1", "format version");
    expect_contains(dump, "lifetime legacy-arc", "lifetime model");
    expect_contains(dump, "declare result : int [primitive]", "typed declaration");
    expect_contains(dump, "binary + : int [primitive]", "typed arithmetic");
    expect_contains(dump, "loop", "loop operation");
    expect_contains(dump, "branch", "branch operation");
    expect_contains(dump, "return", "return operation");
    expect_contains(dump, "@<direct>:2:5", "source origin");
}

void test_struct_fields_defaults_and_access()
{
    auto program = parse_and_check(
        "struct Point {\n"
        "    x: int = 1\n"
        "    y: int\n"
        "}\n"
        "function main() {\n"
        "    point := Point(y: 2)\n"
        "    print(point.x)\n"
        "}\n");
    const auto dump = toro::dump_ir(toro::IRLowerer().lower(program));

    expect_contains(dump, "struct Point", "struct declaration");
    expect_contains(dump, "field x: int [primitive]", "typed struct field");
    expect_contains(dump, "constant 1 : int [primitive]", "field default");
    expect_contains(dump, "construct Point", "struct construction");
    expect_contains(dump, "arguments(y)", "named argument identity");
    expect_contains(dump, "member-access x", "struct member access");
}

void test_representative_feature_inventory_and_determinism()
{
    const auto root = std::filesystem::path(TORO_TEST_SOURCE_DIR);
    auto program = load_and_check(root / "examples/backend_hardening.toro");
    auto first_ir = toro::IRLowerer().lower(program);
    auto second_ir = toro::IRLowerer().lower(program);
    const auto first_dump = toro::dump_ir(first_ir);
    const auto second_dump = toro::dump_ir(second_ir);

    expect(first_dump == second_dump, "repeated IR lowering was not deterministic");
    expect_contains(first_dump, "interface-call label", "interface dispatch");
    expect_contains(first_dump, "virtual-call label", "virtual dispatch");
    expect_contains(first_dump, "generic <T>", "generic declaration");
    expect_contains(first_dump, "<T=int [primitive]>", "generic substitution");
    expect_contains(first_dump, "convert string", "explicit conversion");
    expect_contains(first_dump, "collection-op", "collection operation");
    expect_contains(first_dump, "result-construct ok", "Result construction");
    expect_contains(first_dump, "enum-construct Event::value", "enum construction");
    expect_contains(first_dump, "handle", "handle operation");
    expect_contains(first_dump, "call print", "builtin call");
    expect_contains(
        first_dump,
        "declare named : Named {module__17_backend_hardening__5_Named} [interface]",
        "declared interface type");

    toro::EffectAnalyzer().analyze(first_ir);
    toro::OwnershipSummaryAnalyzer().analyze(first_ir);
    toro::ArcMaterializer().materialize(first_ir);
    toro::OwnershipTransferAnalyzer().analyze(first_ir);
    toro::BorrowInferenceAnalyzer().analyze(first_ir);
    toro::ArcOptimizer().optimize(first_ir);
    toro::EffectAnalyzer().analyze(second_ir);
    toro::OwnershipSummaryAnalyzer().analyze(second_ir);
    toro::ArcMaterializer().materialize(second_ir);
    toro::OwnershipTransferAnalyzer().analyze(second_ir);
    toro::BorrowInferenceAnalyzer().analyze(second_ir);
    toro::ArcOptimizer().optimize(second_ir);

    const auto first_c = toro::CGenerator().generate(first_ir);
    const auto second_c = toro::CGenerator().generate(second_ir);
    expect(first_c == second_c, "IR-backed C generation was not deterministic");
}

void test_result_propagation()
{
    const auto root = std::filesystem::path(TORO_TEST_SOURCE_DIR);
    auto program = load_and_check(root / "examples/results.toro");
    const auto dump = toro::dump_ir(toro::IRLowerer().lower(program));
    expect_contains(dump, "propagate-result", "Result propagation");
    expect_contains(dump, "Result<int, string>", "concrete Result type");
}

void test_modules_and_foreign_calls()
{
    const auto root = std::filesystem::path(TORO_TEST_SOURCE_DIR);
    const auto source_root = root / "examples/ffi/src";
    auto program = load_and_check(source_root / "main.toro", source_root);
    const auto dump = toro::dump_ir(toro::IRLowerer().lower(program));

    expect_contains(dump, "module main imports native", "module import");
    expect_contains(dump, "module native", "loaded module");
    expect_contains(dump, "foreign-function", "foreign declaration");
    expect_contains(dump, "foreign-call puts", "foreign call");
}

void test_standard_library_calls()
{
    const auto root = std::filesystem::path(TORO_TEST_SOURCE_DIR);
    const auto source_root = root / "examples/stdlib/src";
    auto program = load_and_check(source_root / "main.toro", source_root);
    const auto dump = toro::dump_ir(toro::IRLowerer().lower(program));

    expect_contains(dump, "module io", "embedded standard-library module");
    expect_contains(dump, "module main imports", "standard-library imports");
    expect_contains(dump, "call __toro_io_write_line", "runtime intrinsic call");
    expect_contains(
        dump,
        "call write_text : Result<bool, string>",
        "typed standard-library call");
}

void test_backend_uses_detached_ir_only()
{
    auto ir = lower_detached(
        "class Box {\n"
        "    public value: int\n"
        "    public function get() -> int { return self.value }\n"
        "}\n"
        "function main() {\n"
        "    box := Box(value: 7)\n"
        "    print(box.get())\n"
        "}\n");
    const auto effects_before = toro::dump_ir(ir, true);
    const auto generated = toro::CGenerator().generate(ir);
    expect_contains(generated, "toro_class_", "IR-only class lowering");
    expect_contains(generated, "toro_method_", "IR-only method lowering");
    expect(effects_before == toro::dump_ir(ir, true),
        "C generation changed effect summaries");
}

} // namespace

int main()
{
    try {
        test_typed_operations_and_control_flow();
        test_struct_fields_defaults_and_access();
        test_representative_feature_inventory_and_determinism();
        test_result_propagation();
        test_modules_and_foreign_calls();
        test_standard_library_calls();
        test_backend_uses_detached_ir_only();
    } catch (const std::exception& error) {
        std::cerr << "IR test failure: " << error.what() << '\n';
        return 1;
    }

    std::cout << "IR tests passed\n";
    return 0;
}
