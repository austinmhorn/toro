#include "toro/EffectAnalyzer.hpp"
#include "toro/IRLowerer.hpp"
#include "toro/Lexer.hpp"
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
    if (!condition) throw std::runtime_error(std::string(message));
}

toro::IRProgram analyze_source(std::string_view source)
{
    auto tokens = toro::Lexer(source).tokenize();
    auto program = toro::Parser(std::move(tokens)).parse_program();
    toro::SemanticAnalyzer().analyze(program);
    toro::TypeChecker().check(program);
    auto ir = toro::IRLowerer().lower(program);
    toro::EffectAnalyzer().analyze(ir);
    ir.backend_compatibility_program = nullptr;
    return ir;
}

toro::IRProgram analyze_file(
    const std::filesystem::path& entry,
    const std::filesystem::path& source_root = {})
{
    auto program = source_root.empty()
        ? toro::ModuleLoader().load(entry)
        : toro::ModuleLoader().load(entry, source_root);
    toro::SemanticAnalyzer().analyze(program);
    toro::TypeChecker().check(program);
    auto ir = toro::IRLowerer().lower(program);
    toro::EffectAnalyzer().analyze(ir);
    ir.backend_compatibility_program = nullptr;
    return ir;
}

const toro::IRCallableEffectSummary& summary(
    const toro::IRProgram& program,
    std::string_view identity_suffix)
{
    for (const auto& candidate : program.effect_summaries) {
        if (candidate.callable_identity.ends_with(identity_suffix)) return candidate;
    }
    throw std::runtime_error(
        "missing effect summary ending in '" + std::string(identity_suffix) + "'");
}

bool has_effect(
    const toro::IRCallableEffectSummary& callable,
    toro::IREffect effect)
{
    for (const auto candidate : callable.effects) {
        if (candidate == effect) return true;
    }
    return false;
}

const toro::IREffectCause& cause(
    const toro::IRCallableEffectSummary& callable,
    toro::IREffect effect)
{
    for (const auto& candidate : callable.causes) {
        if (candidate.effect == effect) return candidate;
    }
    throw std::runtime_error("missing effect cause");
}

void test_pure_values_references_and_collections()
{
    const auto ir = analyze_source(
        "struct Pair {\n"
        "    left: int\n"
        "    right: int\n"
        "}\n"
        "class Box {\n"
        "    public value: int\n"
        "}\n"
        "function pure(value: int) -> int {\n"
        "    return value + 1\n"
        "}\n"
        "function value_ops() {\n"
        "    pair := Pair(left: 1, right: 2)\n"
        "    pair.left = 3\n"
        "}\n"
        "function reference_ops() -> Box {\n"
        "    first := Box(value: 1)\n"
        "    second := first\n"
        "    return second\n"
        "}\n"
        "function collection_ops() {\n"
        "    values := List<int>(1)\n"
        "    values.add(2)\n"
        "}\n");

    expect(summary(ir, "pure(int)").effects.empty(),
        "primitive arithmetic should be effect-free");
    expect(summary(ir, "value_ops()").effects.empty(),
        "struct value operations should be effect-free");
    const auto& reference = summary(ir, "reference_ops()");
    expect(has_effect(reference, toro::IREffect::Allocate),
        "class construction should allocate");
    expect(has_effect(reference, toro::IREffect::Retain),
        "class references should retain");
    expect(has_effect(reference, toro::IREffect::ReleaseDestroy),
        "class references should release or destroy");
    expect(has_effect(reference, toro::IREffect::Escape),
        "returned class reference should escape");
    const auto& collection = summary(ir, "collection_ops()");
    expect(has_effect(collection, toro::IREffect::Allocate),
        "collection construction/growth should allocate");
    expect(has_effect(collection, toro::IREffect::Retain),
        "shared collection should retain");
    expect(has_effect(collection, toro::IREffect::ReleaseDestroy),
        "shared collection should release");
}

void test_transitive_recursive_and_mutual_fixed_point()
{
    constexpr std::string_view source =
        "function output() {\n"
        "    print(\"effect\")\n"
        "}\n"
        "function caller() {\n"
        "    output()\n"
        "}\n"
        "function recursive(value: int) {\n"
        "    if value > 0 {\n"
        "        recursive(value - 1)\n"
        "    } else {\n"
        "        output()\n"
        "    }\n"
        "}\n"
        "function first(value: int) {\n"
        "    if value > 0 {\n"
        "        second(value - 1)\n"
        "    }\n"
        "}\n"
        "function second(value: int) {\n"
        "    print(value)\n"
        "    if value > 0 {\n"
        "        first(value - 1)\n"
        "    }\n"
        "}\n";
    const auto ir = analyze_source(source);

    for (const auto name : {"caller()", "recursive(int)", "first(int)",
             "second(int)"}) {
        expect(has_effect(summary(ir, name), toro::IREffect::Block),
            std::string(name) + " did not receive transitive blocking I/O");
    }
    const auto& caller_cause = cause(summary(ir, "caller()"), toro::IREffect::Block);
    expect(caller_cause.transitive && caller_cause.via_callable == "output()",
        "direct transitive cause was not retained");
    expect(cause(summary(ir, "recursive(int)"), toro::IREffect::Block)
                   .via_callable
            == "output()",
        "recursive summary selected an unhelpful self cause");

    auto second = analyze_source(source);
    expect(toro::dump_ir(ir, true) == toro::dump_ir(second, true),
        "fixed-point effect output was not deterministic");
}

void test_generics_and_dynamic_dispatch()
{
    const auto ir = analyze_source(
        "interface Named {\n"
        "    function label() -> string\n"
        "}\n"
        "class Base {\n"
        "    public virtual function speak() {\n"
        "        print(\"base\")\n"
        "    }\n"
        "}\n"
        "class Item : Base implements Named {\n"
        "    public override function speak() {\n"
        "        print(\"item\")\n"
        "    }\n"
        "    public function label() -> string {\n"
        "        print(\"label\")\n"
        "        return \"item\"\n"
        "    }\n"
        "}\n"
        "function make_list<T>(value: T) -> List<T> {\n"
        "    return List<T>(value)\n"
        "}\n"
        "function identity<T>(value: T) -> T { return value }\n"
        "function virtual_call(value: Base) { value.speak() }\n"
        "function interface_call(value: Named) { value.label() }\n"
        "function main() {\n"
        "    values := make_list<int>(1)\n"
        "    number := identity<int>(1)\n"
        "    item := Item()\n"
        "    copy := identity<Item>(item)\n"
        "    virtual_call(item)\n"
        "    interface_call(item)\n"
        "}\n");

    const auto& specialization = summary(ir, "make_list(T)<T=int>");
    expect(has_effect(specialization, toro::IREffect::Allocate),
        "generic specialization lost allocation effect");
    expect(summary(ir, "identity(T)<T=int>").effects.empty(),
        "primitive generic specialization gained reference-lifetime effects");
    const auto& class_specialization = summary(ir, "identity(T)<T=Item>");
    expect(has_effect(class_specialization, toro::IREffect::Retain)
            && has_effect(class_specialization, toro::IREffect::ReleaseDestroy)
            && has_effect(class_specialization, toro::IREffect::Escape),
        "generic class-reference specialization lost lifetime effects");
    const auto& virtual_call = summary(ir, "virtual_call(Base)");
    expect(has_effect(virtual_call, toro::IREffect::DynamicDispatch),
        "virtual call was not marked dynamic");
    expect(has_effect(virtual_call, toro::IREffect::Block),
        "virtual implementation effects did not propagate");
    const auto& interface_call = summary(ir, "interface_call(Named)");
    expect(has_effect(interface_call, toro::IREffect::DynamicDispatch),
        "interface call was not marked dynamic");
    expect(has_effect(interface_call, toro::IREffect::Block),
        "interface implementation effects did not propagate");
}

void test_unresolved_dynamic_dispatch_is_unknown()
{
    const auto ir = analyze_source(
        "interface Action {\n"
        "    function run()\n"
        "}\n"
        "function invoke(value: Action) {\n"
        "    value.run()\n"
        "}\n");
    const auto& invoke = summary(ir, "invoke(Action)");
    expect(has_effect(invoke, toro::IREffect::DynamicDispatch),
        "unresolved interface call was not dynamic");
    expect(has_effect(invoke, toro::IREffect::UnknownExternal),
        "unresolved interface implementation was treated as harmless");
}

void test_standard_library_and_foreign_metadata()
{
    const auto root = std::filesystem::path(TORO_TEST_SOURCE_DIR);
    const auto stdlib_root = root / "examples/stdlib/src";
    const auto stdlib = analyze_file(stdlib_root / "main.toro", stdlib_root);

    const auto& concat = summary(stdlib, "__6_concat(string,string)");
    expect(has_effect(concat, toro::IREffect::Allocate),
        "string concat should allocate");
    expect(has_effect(concat, toro::IREffect::NativeCall),
        "string concat should cross the runtime boundary");
    const auto& read = summary(stdlib, "__9_read_text(string)");
    expect(has_effect(read, toro::IREffect::Block)
            && has_effect(read, toro::IREffect::FilesystemIO),
        "filesystem read effects were incomplete");
    const auto& env = summary(stdlib, "__3_has(string)");
    expect(has_effect(env, toro::IREffect::EnvironmentAccess),
        "environment access was not classified");
    const auto& time = summary(stdlib, "__11_wall_millis()");
    expect(has_effect(time, toro::IREffect::TimeQuery),
        "wall clock query was not classified");
    expect(!has_effect(time, toro::IREffect::FilesystemIO),
        "wall clock query was misclassified as filesystem I/O");

    const auto ffi_root = root / "examples/ffi/src";
    const auto ffi = analyze_file(ffi_root / "main.toro", ffi_root);
    const auto& puts = summary(ffi, "__4_puts(c_string)");
    expect(has_effect(puts, toro::IREffect::ForeignCall),
        "foreign call was not classified");
    expect(has_effect(puts, toro::IREffect::UnknownExternal),
        "foreign call was not conservatively unknown");
    expect(has_effect(summary(ffi, "__4_main()"),
               toro::IREffect::UnknownExternal),
        "unknown foreign effects did not propagate");
}

void test_process_termination_intrinsic()
{
    toro::IRProgram program;
    toro::IRDeclaration declaration;
    declaration.kind = toro::IRDeclarationKind::Function;
    declaration.identity = "terminate()";
    declaration.name = "terminate";
    declaration.origin = {"probe", {1, 1}};
    toro::IROperation body;
    body.kind = toro::IROperationKind::Block;
    body.origin = {"probe", {1, 22}};
    toro::IROperation call;
    call.kind = toro::IROperationKind::Call;
    call.name = "__toro_process_exit";
    call.origin = {"probe", {2, 5}};
    body.children.push_back(std::move(call));
    declaration.body = std::move(body);
    program.declarations.push_back(std::move(declaration));

    toro::EffectAnalyzer().analyze(program);
    const auto& terminate = summary(program, "terminate()");
    expect(has_effect(terminate, toro::IREffect::Terminate),
        "process exit should be terminating");
    expect(has_effect(terminate, toro::IREffect::NativeCall)
            && has_effect(terminate, toro::IREffect::ProcessIO),
        "process exit runtime effects were incomplete");
    const auto& terminate_cause = cause(terminate, toro::IREffect::Terminate);
    expect(terminate_cause.origin.module_name == "probe"
            && terminate_cause.origin.location.line == 2,
        "effect source origin was not retained");
}

} // namespace

int main()
{
    try {
        test_pure_values_references_and_collections();
        test_transitive_recursive_and_mutual_fixed_point();
        test_generics_and_dynamic_dispatch();
        test_unresolved_dynamic_dispatch_is_unknown();
        test_standard_library_and_foreign_metadata();
        test_process_termination_intrinsic();
    } catch (const std::exception& error) {
        std::cerr << "effect analyzer test failure: " << error.what() << '\n';
        return 1;
    }

    std::cout << "effect analyzer tests passed\n";
    return 0;
}
