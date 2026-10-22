#include "toro/Diagnostic.hpp"
#include "toro/EffectAnalyzer.hpp"
#include "toro/IRLowerer.hpp"
#include "toro/Lexer.hpp"
#include "toro/Parser.hpp"
#include "toro/PerformanceContractAnalyzer.hpp"
#include "toro/SemanticAnalyzer.hpp"
#include "toro/TypeChecker.hpp"

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

toro::IRProgram analyze(std::string_view source)
{
    auto tokens = toro::Lexer(source).tokenize();
    auto program = toro::Parser(std::move(tokens)).parse_program();
    toro::SemanticAnalyzer().analyze(program);
    toro::TypeChecker().check(program);
    auto ir = toro::IRLowerer().lower(program);
    toro::EffectAnalyzer().analyze(ir);
    toro::PerformanceContractAnalyzer().analyze(ir);
    return ir;
}

const toro::IRPerformanceContractResult& result(
    const toro::IRProgram& program,
    std::string_view identity_suffix)
{
    for (const auto& candidate : program.performance_contract_results) {
        if (candidate.callable_identity.ends_with(identity_suffix)) return candidate;
    }
    throw std::runtime_error(
        "missing performance result ending in '"
        + std::string(identity_suffix) + "'");
}

bool has_cause(
    const toro::IRPerformanceContractResult& contract,
    toro::IREffect effect,
    bool transitive = false)
{
    for (const auto& cause : contract.causes) {
        if (cause.effect == effect && (!transitive || cause.transitive)) return true;
    }
    return false;
}

std::string diagnostics(const toro::IRProgram& program)
{
    std::string output;
    for (const auto& diagnostic : program.diagnostics) {
        output += toro::format_diagnostic(diagnostic);
    }
    return output;
}

void test_pure_contract_and_ordinary_function()
{
    const auto ir = analyze(
        "class Box {}\n"
        "performance function calculate(value: int) -> int {\n"
        "    return value * 2 + 1\n"
        "}\n"
        "function ordinary() {\n"
        "    box := Box()\n"
        "    print(1)\n"
        "}\n");
    expect(result(ir, "calculate(int)").status
            == toro::IRPerformanceContractStatus::Satisfied,
        "pure performance function did not satisfy its contract");
    expect(ir.performance_contract_results.size() == 1,
        "ordinary function was performance-validated");
    expect(!toro::has_error_diagnostics(ir.diagnostics),
        "ordinary function effects produced a contract error");
}

void test_direct_effects_and_independent_results()
{
    const auto ir = analyze(
        "class Box {}\n"
        "performance function allocate() { value := Box() }\n"
        "performance function output() { print(1) }\n"
        "performance function pure(value: int) -> int { return value + 1 }\n");
    const auto& allocation = result(ir, "allocate()");
    expect(allocation.status == toro::IRPerformanceContractStatus::Violated
            && has_cause(allocation, toro::IREffect::Allocate),
        "heap allocation was not prohibited");
    const auto& output = result(ir, "output()");
    expect(output.status == toro::IRPerformanceContractStatus::Violated
            && has_cause(output, toro::IREffect::Block),
        "blocking output was not prohibited");
    expect(result(ir, "pure(int)").status
            == toro::IRPerformanceContractStatus::Satisfied,
        "independent pure callable was contaminated by another violation");
}

void test_transitive_effects_reuse_existing_summary()
{
    const auto ir = analyze(
        "function helper() { print(\"blocked\") }\n"
        "performance function wrapper() { helper() }\n");
    const auto& wrapper = result(ir, "wrapper()");
    expect(wrapper.status == toro::IRPerformanceContractStatus::Violated,
        "transitive blocking effect was accepted");
    expect(has_cause(wrapper, toro::IREffect::Block, true),
        "transitive effect cause was not retained");
    const auto text = diagnostics(ir);
    expect(text.find("call to 'helper'") != std::string::npos
            && text.find("blocking operation") != std::string::npos,
        "transitive diagnostic omitted callable or effect context");
}

void test_unknown_foreign_behavior_is_unproven()
{
    const auto ir = analyze(
        "extern \"C\" function mystery() -> c_int\n"
        "performance function probe() { value := mystery() }\n");
    const auto& probe = result(ir, "probe()");
    expect(probe.status == toro::IRPerformanceContractStatus::Unproven,
        "unknown foreign behavior was not distinguished from a violation");
    expect(has_cause(probe, toro::IREffect::UnknownExternal, true),
        "unknown foreign cause was not propagated");
    const auto text = diagnostics(ir);
    expect(text.find("cannot be proven") != std::string::npos
            && text.find("unknown external behavior") != std::string::npos,
        "unproven diagnostic was not clear");
}

void test_recursive_and_dynamic_effect_summaries()
{
    const auto recursive = analyze(
        "function output() { print(1) }\n"
        "performance function repeat(value: int) {\n"
        "    if value > 0 { repeat(value - 1) } else { output() }\n"
        "}\n");
    expect(result(recursive, "repeat(int)").status
            == toro::IRPerformanceContractStatus::Violated,
        "recursive transitive effect did not violate the contract");

    const auto safe_dispatch = analyze(
        "interface Action { function run() }\n"
        "class Safe implements Action { public function run() {} }\n"
        "performance function invoke(value: Action) { value.run() }\n");
    expect(result(safe_dispatch, "invoke(Action)").status
            == toro::IRPerformanceContractStatus::Satisfied,
        "fully known safe interface dispatch was rejected");

    const auto unsafe_dispatch = analyze(
        "interface Action { function run() }\n"
        "class Unsafe implements Action {\n"
        "    public function run() { print(1) }\n"
        "}\n"
        "performance function invoke(value: Action) { value.run() }\n");
    expect(result(unsafe_dispatch, "invoke(Action)").status
            == toro::IRPerformanceContractStatus::Violated,
        "unsafe interface target effect did not propagate");

    const auto unresolved_dispatch = analyze(
        "interface Action { function run() }\n"
        "performance function invoke(value: Action) { value.run() }\n");
    expect(result(unresolved_dispatch, "invoke(Action)").status
            == toro::IRPerformanceContractStatus::Unproven,
        "unresolved interface dispatch was not conservative");
}

void test_generic_specializations_and_method_contracts()
{
    const auto ir = analyze(
        "class Box {}\n"
        "class Calculator {\n"
        "    public performance function add(value: int) -> int {\n"
        "        return value + 1\n"
        "    }\n"
        "}\n"
        "performance function identity<T>(value: T) -> T { return value }\n"
        "function main() {\n"
        "    number := identity<int>(1)\n"
        "    box := Box()\n"
        "    same := identity<Box>(box)\n"
        "}\n");
    expect(result(ir, "Calculator::add(int)").status
            == toro::IRPerformanceContractStatus::Satisfied,
        "performance method did not retain its contract");
    expect(result(ir, "identity(T)<T=int>").status
            == toro::IRPerformanceContractStatus::Satisfied,
        "safe generic specialization failed");
    const auto& managed = result(ir, "identity(T)<T=Box>");
    expect(managed.status == toro::IRPerformanceContractStatus::Violated
            && has_cause(managed, toro::IREffect::Escape),
        "managed generic specialization lost its distinct effect policy");
}

void test_synchronization_policy_and_determinism()
{
    toro::IRProgram first;
    toro::IRDeclaration declaration;
    declaration.kind = toro::IRDeclarationKind::Function;
    declaration.name = "locked";
    declaration.identity = "locked()";
    declaration.origin = {"probe", {1, 1}};
    declaration.performance_contract = toro::PerformanceContract::Performance;
    first.declarations.push_back(declaration);
    first.effect_summaries.push_back(toro::IRCallableEffectSummary{
        "locked()",
        {toro::IREffect::Synchronize},
        {{toro::IREffect::Synchronize, {"probe", {2, 5}}, {}, false}},
    });
    toro::PerformanceContractAnalyzer().analyze(first);
    auto second = first;
    toro::PerformanceContractAnalyzer().analyze(second);
    expect(result(first, "locked()").status
            == toro::IRPerformanceContractStatus::Violated,
        "synchronization was not prohibited");
    expect(diagnostics(first) == diagnostics(second),
        "performance diagnostics were not deterministic or idempotent");
}

void test_non_prohibited_effect_categories_remain_allowed()
{
    toro::IRProgram ir;
    toro::IRDeclaration declaration;
    declaration.kind = toro::IRDeclarationKind::Function;
    declaration.name = "observed";
    declaration.identity = "observed()";
    declaration.origin = {"probe", {1, 1}};
    declaration.performance_contract = toro::PerformanceContract::Performance;
    ir.declarations.push_back(declaration);
    ir.effect_summaries.push_back(toro::IRCallableEffectSummary{
        "observed()",
        {
            toro::IREffect::Retain,
            toro::IREffect::ReleaseDestroy,
            toro::IREffect::EnvironmentAccess,
            toro::IREffect::TimeQuery,
            toro::IREffect::ProcessIO,
            toro::IREffect::ForeignCall,
            toro::IREffect::NativeCall,
            toro::IREffect::DynamicDispatch,
            toro::IREffect::Terminate,
        },
        {},
    });
    toro::PerformanceContractAnalyzer().analyze(ir);
    expect(result(ir, "observed()").status
            == toro::IRPerformanceContractStatus::Satisfied,
        "a represented but non-prohibited effect was rejected");
    expect(!toro::has_error_diagnostics(ir.diagnostics),
        "allowed effects produced a performance-contract diagnostic");
}

} // namespace

int main()
{
    try {
        test_pure_contract_and_ordinary_function();
        test_direct_effects_and_independent_results();
        test_transitive_effects_reuse_existing_summary();
        test_unknown_foreign_behavior_is_unproven();
        test_recursive_and_dynamic_effect_summaries();
        test_generic_specializations_and_method_contracts();
        test_synchronization_policy_and_determinism();
        test_non_prohibited_effect_categories_remain_allowed();
    } catch (const std::exception& error) {
        std::cerr << "performance contract test failure: " << error.what() << '\n';
        return 1;
    }
    std::cout << "performance contract tests passed\n";
    return 0;
}
