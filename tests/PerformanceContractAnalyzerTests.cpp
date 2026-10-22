#include "toro/ArcOptimizer.hpp"
#include "toro/BorrowInferenceAnalyzer.hpp"
#include "toro/CGenerator.hpp"
#include "toro/Diagnostic.hpp"
#include "toro/EffectAnalyzer.hpp"
#include "toro/EscapeAnalyzer.hpp"
#include "toro/IRLowerer.hpp"
#include "toro/Lexer.hpp"
#include "toro/OwnershipSummaryAnalyzer.hpp"
#include "toro/Parser.hpp"
#include "toro/PerformanceContractAnalyzer.hpp"
#include "toro/SemanticAnalyzer.hpp"
#include "toro/StorageDecisionAnalyzer.hpp"
#include "toro/StrongCycleAnalyzer.hpp"
#include "toro/TypeChecker.hpp"

#include <algorithm>
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

toro::IRProgram prepare(
    std::string_view source,
    bool enable_stack = true,
    bool enable_borrowing = true)
{
    auto tokens = toro::Lexer(source).tokenize();
    auto program = toro::Parser(std::move(tokens)).parse_program();
    toro::SemanticAnalyzer().analyze(program);
    toro::TypeChecker().check(program);
    auto ir = toro::IRLowerer().lower(program);
    toro::EffectAnalyzer().analyze(ir);
    toro::OwnershipSummaryAnalyzer().analyze(ir);
    toro::StrongCycleAnalyzer().analyze(ir);
    toro::EscapeAnalyzer().analyze(ir);
    toro::StorageDecisionAnalyzer().analyze(ir, enable_stack);
    toro::ArcMaterializer().materialize(ir);
    toro::OwnershipTransferAnalyzer().analyze(ir);
    toro::BorrowInferenceAnalyzer().analyze(ir, enable_borrowing);
    toro::ArcOptimizer().optimize(ir);
    return ir;
}

toro::IRProgram analyze(
    std::string_view source,
    bool enable_stack = true,
    bool enable_borrowing = true)
{
    auto ir = prepare(source, enable_stack, enable_borrowing);
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

const toro::IRAllocationEscape& allocation(
    const toro::IRProgram& program,
    std::string_view callable_suffix,
    std::size_t occurrence = 0)
{
    for (const auto& candidate : program.escape_allocations) {
        if (!candidate.callable_identity.ends_with(callable_suffix)) continue;
        if (occurrence == 0) return candidate;
        --occurrence;
    }
    throw std::runtime_error("missing managed allocation");
}

const toro::IRCallableArcSchedule& schedule(
    const toro::IRProgram& program,
    std::string_view callable_suffix)
{
    for (const auto& candidate : program.arc_schedules) {
        if (candidate.callable_identity.ends_with(callable_suffix)) return candidate;
    }
    throw std::runtime_error("missing ARC schedule");
}

bool has_effect(
    const toro::IRProgram& program,
    std::string_view callable_suffix,
    toro::IREffect effect)
{
    for (const auto& summary : program.effect_summaries) {
        if (!summary.callable_identity.ends_with(callable_suffix)) continue;
        return std::ranges::find(summary.effects, effect)
            != summary.effects.end();
    }
    return false;
}

bool has_stack_refinement(
    const toro::IRPerformanceContractResult& contract,
    bool transitive = false)
{
    for (const auto& refinement : contract.refinements) {
        if (refinement.effect == toro::IREffect::Allocate
            && refinement.storage == toro::IRStorageClass::Stack
            && (!transitive || refinement.transitive)) {
            return true;
        }
    }
    return false;
}

bool has_transfer_annotation(const toro::IROperation& operation)
{
    if (operation.ownership_transfer) return true;
    return std::ranges::any_of(operation.children,
        [](const auto& child) { return has_transfer_annotation(child); });
}

bool callable_has_transfer(
    const toro::IRProgram& program,
    std::string_view callable_suffix)
{
    for (const auto& declaration : program.declarations) {
        if (declaration.identity.ends_with(callable_suffix)
            && declaration.body) {
            return has_transfer_annotation(*declaration.body);
        }
    }
    return false;
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
        "performance function allocate() -> Box { return Box() }\n"
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

void test_stack_storage_refines_allocation_policy()
{
    constexpr std::string_view source =
        "class Particle {\n"
        "    public value: int = 0\n"
        "    public function tick() { self.value = self.value + 1 }\n"
        "}\n"
        "performance function update() {\n"
        "    particle := Particle()\n"
        "    particle.tick()\n"
        "}\n";
    const auto optimized = analyze(source);
    const auto heap_only = analyze(source, false);
    expect(has_effect(optimized, "update()", toro::IREffect::Allocate),
        "storage refinement rewrote the semantic effect summary");
    expect(allocation(optimized, "update()").state
                == toro::IREscapeState::DoesNotEscape
            && allocation(optimized, "update()").storage
                == toro::IRStorageClass::Stack,
        "safe performance allocation was not stack promoted");
    expect(result(optimized, "update()").status
                == toro::IRPerformanceContractStatus::Satisfied
            && has_stack_refinement(result(optimized, "update()")),
        "stack-backed construction did not satisfy the contract");
    expect(allocation(heap_only, "update()").storage
                == toro::IRStorageClass::Heap
            && result(heap_only, "update()").status
                == toro::IRPerformanceContractStatus::Violated
            && has_cause(result(heap_only, "update()"),
                toro::IREffect::Allocate),
        "heap-only runtime plan did not preserve the allocation violation");

    const auto generated = toro::CGenerator().generate(optimized);
    expect(generated.find("toro_new_class_0_storage") != std::string::npos,
        "accepted stack allocation did not emit automatic object storage");
    expect(generated.find("toro_new_class_0 = malloc") == std::string::npos,
        "accepted stack allocation still emitted allocation-site malloc");
}

void test_heap_escape_multiple_sites_and_collections()
{
    const auto returned = analyze(
        "class Particle {}\n"
        "performance function make() -> Particle { return Particle() }\n");
    expect(allocation(returned, "make()").storage
                == toro::IRStorageClass::Heap
            && has_cause(result(returned, "make()"), toro::IREffect::Allocate)
            && has_cause(result(returned, "make()"), toro::IREffect::Escape),
        "escaping return did not retain heap-allocation and escape violations");

    const auto mixed = analyze(
        "class Particle { public function tick() {} }\n"
        "class Watcher { public weak target: Particle? }\n"
        "performance function work(watcher: Watcher) {\n"
        "    first := Particle()\n"
        "    second := Particle()\n"
        "    watcher.target = second\n"
        "    first.tick()\n"
        "}\n");
    expect(mixed.escape_metrics.stack_allocations == 1
            && mixed.escape_metrics.heap_allocations == 1,
        "mixed allocation sites did not receive independent storage decisions");
    expect(has_stack_refinement(result(mixed, "work(Watcher)"))
            && has_cause(result(mixed, "work(Watcher)"),
                toro::IREffect::Allocate),
        "one stack site incorrectly discharged a separate heap site");

    const auto collection = analyze(
        "performance function collect() { values := List<int>() }\n");
    expect(result(collection, "collect()").status
                == toro::IRPerformanceContractStatus::Violated
            && has_cause(result(collection, "collect()"),
                toro::IREffect::Allocate),
        "runtime collection allocation was incorrectly discharged");

    const auto persistent = analyze(
        "class Particle {}\n"
        "class Holder { public value: Particle? }\n"
        "performance function store(holder: Holder) {\n"
        "    value := Particle()\n"
        "    holder.value = value\n"
        "}\n");
    expect(allocation(persistent, "store(Holder)").storage
                == toro::IRStorageClass::Heap
            && has_cause(result(persistent, "store(Holder)"),
                toro::IREffect::Escape),
        "persistent strong storage was incorrectly treated as local");
}

void test_transitive_runtime_plan_refinement()
{
    const auto safe = analyze(
        "class Temporary { public function work() {} }\n"
        "function helper() {\n"
        "    value := Temporary()\n"
        "    value.work()\n"
        "}\n"
        "performance function tick() { helper() }\n");
    expect(result(safe, "tick()").status
                == toro::IRPerformanceContractStatus::Satisfied
            && has_stack_refinement(result(safe, "tick()"), true),
        "transitive stack-only helper did not satisfy the contract");

    const auto unsafe = analyze(
        "class Temporary {}\n"
        "function helper() -> Temporary { return Temporary() }\n"
        "performance function tick() { value := helper() }\n");
    expect(result(unsafe, "tick()").status
                == toro::IRPerformanceContractStatus::Violated
            && has_cause(result(unsafe, "tick()"),
                toro::IREffect::Allocate, true),
        "transitive heap-backed helper was accepted");
}

void test_final_ownership_borrow_and_dispatch_facts()
{
    const auto ir = analyze(
        "interface Runnable { function run() }\n"
        "class Base { public virtual function run() {} }\n"
        "class Particle : Base implements Runnable {\n"
        "    public value: int\n"
        "    public override function run() { self.value = self.value + 1 }\n"
        "}\n"
        "class Box<T> { public value: T }\n"
        "function inspect(value: Particle) -> int { return value.value }\n"
        "performance function borrowed(value: Particle) {\n"
        "    seen := inspect(value)\n"
        "}\n"
        "performance function aliasing() {\n"
        "    value := Particle()\n"
        "    alias := value\n"
        "    alias.run()\n"
        "    value.run()\n"
        "}\n"
        "performance function transferred(value: Particle?) {\n"
        "    copy := value\n"
        "}\n"
        "performance function virtual_call() {\n"
        "    value := Particle()\n"
        "    base: Base = value\n"
        "    base.run()\n"
        "}\n"
        "performance function interface_call() {\n"
        "    value := Particle()\n"
        "    runnable: Runnable = value\n"
        "    runnable.run()\n"
        "}\n"
        "performance function generic_value() {\n"
        "    value := Box<int>(value: 1)\n"
        "    seen := value.value\n"
        "}\n");
    for (const auto suffix : {
             "borrowed(Particle)",
             "aliasing()",
             "transferred(Particle?)",
             "virtual_call()",
             "interface_call()",
             "generic_value()",
         }) {
        expect(result(ir, suffix).status
                == toro::IRPerformanceContractStatus::Satisfied,
            "a safe final ownership/dispatch plan violated its contract");
    }
    expect(ir.arc_stage == toro::IRArcStage::Optimized
            && ir.arc_metrics.borrows_inferred != 0
            && ir.arc_metrics.retains_after < ir.arc_metrics.retains_before,
        "performance validation did not consume a final optimized ARC plan");
    expect(std::ranges::any_of(
               schedule(ir, "borrowed(Particle)").optimized_operations,
               [](const auto& operation) {
                   return operation.kind
                       == toro::IRArcOperationKind::BorrowedTemporary;
               }),
        "observation-only call was not represented as borrowed");
    expect(callable_has_transfer(ir, "transferred(Particle?)"),
        "final-use ownership transfer was not preserved before validation");
    expect(ir.escape_metrics.stack_allocations == 4,
        "safe alias/dispatch/generic constructions were not stack-backed");
}

void test_cross_analysis_invariants()
{
    auto ir = prepare(
        "class Item { public value: int }\n"
        "performance function work() { item := Item(value: 1) }\n");
    const auto effects_before = toro::dump_ir(ir, true, false, false, false);
    const auto ownership_before = toro::dump_ir(ir, false, false, false, true);
    const auto escape_before = toro::dump_ir(ir, false, false, true, false);
    const auto arc_before = toro::dump_ir(ir, false, true, false, false);
    const auto cycles_before = ir.strong_reference_cycles.size();
    const auto allocation_before = ir.escape_allocations.front();
    const auto arc_metrics_before = ir.arc_metrics;
    toro::PerformanceContractAnalyzer().analyze(ir);
    expect(effects_before == toro::dump_ir(ir, true, false, false, false)
            && ownership_before
                == toro::dump_ir(ir, false, false, false, true)
            && escape_before == toro::dump_ir(ir, false, false, true, false)
            && arc_before == toro::dump_ir(ir, false, true, false, false),
        "performance validation mutated an upstream analysis dump");
    expect(ir.strong_reference_cycles.size() == cycles_before
            && ir.escape_allocations.front().identity
                == allocation_before.identity
            && ir.escape_allocations.front().state == allocation_before.state
            && ir.escape_allocations.front().storage
                == allocation_before.storage
            && ir.arc_metrics.retains_after
                == arc_metrics_before.retains_after
            && ir.arc_metrics.releases_after
                == arc_metrics_before.releases_after,
        "performance validation mutated upstream analysis facts");
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
    first.arc_stage = toro::IRArcStage::Optimized;
    first.effect_summaries.push_back(toro::IRCallableEffectSummary{
        "locked()",
        {toro::IREffect::FilesystemIO, toro::IREffect::Synchronize},
        {
            {toro::IREffect::FilesystemIO, {"probe", {2, 5}}, {}, false},
            {toro::IREffect::Synchronize, {"probe", {3, 5}}, {}, false},
        },
        {
            {toro::IREffect::FilesystemIO,
             {"probe", {2, 5}},
             std::nullopt,
             {}},
            {toro::IREffect::Synchronize,
             {"probe", {3, 5}},
             std::nullopt,
             {}},
        },
        {},
    });
    toro::PerformanceContractAnalyzer().analyze(first);
    auto second = first;
    toro::PerformanceContractAnalyzer().analyze(second);
    expect(result(first, "locked()").status
            == toro::IRPerformanceContractStatus::Violated,
        "synchronization was not prohibited");
    expect(has_cause(result(first, "locked()"), toro::IREffect::FilesystemIO),
        "filesystem I/O was not prohibited");
    expect(has_cause(result(first, "locked()"), toro::IREffect::Synchronize),
        "synchronization cause was not retained");
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
    ir.arc_stage = toro::IRArcStage::Optimized;
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
        {},
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
        test_stack_storage_refines_allocation_policy();
        test_heap_escape_multiple_sites_and_collections();
        test_transitive_runtime_plan_refinement();
        test_final_ownership_borrow_and_dispatch_facts();
        test_cross_analysis_invariants();
        test_synchronization_policy_and_determinism();
        test_non_prohibited_effect_categories_remain_allowed();
    } catch (const std::exception& error) {
        std::cerr << "performance contract test failure: " << error.what() << '\n';
        return 1;
    }
    std::cout << "performance contract tests passed\n";
    return 0;
}
