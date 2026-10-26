#include "toro/ArcOptimizer.hpp"
#include "toro/BorrowInferenceAnalyzer.hpp"
#include "toro/CGenerator.hpp"
#include "toro/EffectAnalyzer.hpp"
#include "toro/EscapeAnalyzer.hpp"
#include "toro/IRLowerer.hpp"
#include "toro/InspectionReporter.hpp"
#include "toro/Lexer.hpp"
#include "toro/OwnershipSummaryAnalyzer.hpp"
#include "toro/Parser.hpp"
#include "toro/PerformanceContractAnalyzer.hpp"
#include "toro/SemanticAnalyzer.hpp"
#include "toro/StorageDecisionAnalyzer.hpp"
#include "toro/StrongCycleAnalyzer.hpp"
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
    toro::OwnershipSummaryAnalyzer().analyze(ir);
    toro::StrongCycleAnalyzer().analyze(ir);
    toro::EscapeAnalyzer().analyze(ir);
    toro::StorageDecisionAnalyzer().analyze(ir);
    toro::ArcMaterializer().materialize(ir);
    toro::OwnershipTransferAnalyzer().analyze(ir);
    toro::BorrowInferenceAnalyzer().analyze(ir);
    toro::ArcOptimizer().optimize(ir);
    toro::PerformanceContractAnalyzer().analyze(ir);
    return ir;
}

void test_structured_runtime_facts()
{
    const auto ir = analyze(
        "interface Action { function run() }\n"
        "class Item implements Action {\n"
        "    public function run() {}\n"
        "}\n"
        "function local() {\n"
        "    item := Item()\n"
        "    action: Action = item\n"
        "    action.run()\n"
        "}\n"
        "function make() -> Item { return Item() }\n"
        "function noisy() { print(1) }\n"
        "function aliases(value: Item) {\n"
        "    copy := value\n"
        "    copy.run()\n"
        "}\n");

    const auto report = toro::InspectionReporter().inspect(ir);
    expect(report.allocations.size() == 2,
        "inspection did not retain both allocation sites");
    expect(report.allocations[0].identity != report.allocations[1].identity,
        "allocation identities were not stable and distinct");
    bool saw_stack = false;
    bool saw_heap = false;
    for (const auto& allocation : report.allocations) {
        expect(allocation.origin.location.line != 0,
            "allocation origin was not retained");
        saw_stack |= allocation.storage == toro::IRStorageClass::Stack;
        saw_heap |= allocation.storage == toro::IRStorageClass::Heap
            && allocation.reason.find("return") != std::string::npos;
    }
    expect(saw_stack && saw_heap,
        "inspection did not distinguish proven and fallback storage");
    expect(report.dispatches.size() == 1
            && report.dispatches.front().operation_kind
                == toro::IROperationKind::InterfaceCall
            && report.dispatches.front().target_identities.size() == 1,
        "interface dispatch targets were not represented structurally");
    expect(!report.arc.empty(), "ARC runtime behavior was not represented");
    expect(report.capture_count == 0,
        "unsupported captures were reported as present");
    expect(!report.effects.empty()
            && report.effects.front().effect == toro::IREffect::Block,
        "blocking effect was not exposed by inspection");
}

void test_determinism_and_non_mutation()
{
    auto ir = analyze(
        "class Box<T> { public value: T }\n"
        "function work() {\n"
        "    box := Box<int>(value: 1)\n"
        "    print(box.value)\n"
        "}\n");
    const auto effects_before = toro::dump_ir(ir, true, false, false, false);
    const auto ownership_before = toro::dump_ir(ir, false, false, false, true);
    const auto escape_before = toro::dump_ir(ir, false, false, true, false);
    const auto arc_before = toro::dump_ir(ir, false, true, false, false);
    const auto generated_before = toro::CGenerator().generate(ir);
    const auto effect_count = ir.effect_summaries.size();
    const auto ownership_count = ir.ownership_summaries.size();
    const auto cycle_count = ir.strong_reference_cycles.size();
    const auto allocation_count = ir.escape_allocations.size();
    const auto arc_schedule_count = ir.arc_schedules.size();
    const auto performance_count = ir.performance_contract_results.size();

    const auto first = toro::InspectionReporter().report(ir);
    const auto second = toro::InspectionReporter().report(ir);
    expect(first == second, "inspection output was not deterministic");
    expect(first.find("Box<int>") != std::string::npos,
        "concrete generic allocation type was not preserved");
    expect(effects_before == toro::dump_ir(ir, true, false, false, false)
            && ownership_before
                == toro::dump_ir(ir, false, false, false, true)
            && escape_before == toro::dump_ir(ir, false, false, true, false)
            && arc_before == toro::dump_ir(ir, false, true, false, false),
        "inspection mutated an authoritative analysis view");
    expect(effect_count == ir.effect_summaries.size()
            && ownership_count == ir.ownership_summaries.size()
            && cycle_count == ir.strong_reference_cycles.size()
            && allocation_count == ir.escape_allocations.size()
            && arc_schedule_count == ir.arc_schedules.size()
            && performance_count == ir.performance_contract_results.size(),
        "inspection changed authoritative analysis results");
    expect(generated_before == toro::CGenerator().generate(ir),
        "inspection changed generated C");
}

} // namespace

int main()
{
    try {
        test_structured_runtime_facts();
        test_determinism_and_non_mutation();
    } catch (const std::exception& error) {
        std::cerr << "inspection reporter test failure: " << error.what()
                  << '\n';
        return 1;
    }
    std::cout << "inspection reporter tests passed\n";
    return 0;
}
