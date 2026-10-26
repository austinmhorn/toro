#include "toro/ArcOptimizer.hpp"
#include "toro/BorrowInferenceAnalyzer.hpp"
#include "toro/CGenerator.hpp"
#include "toro/EffectAnalyzer.hpp"
#include "toro/EscapeAnalyzer.hpp"
#include "toro/IRLowerer.hpp"
#include "toro/Lexer.hpp"
#include "toro/MemoryReporter.hpp"
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

void expect_summary_matches_details(const toro::MemoryReport& report)
{
    std::size_t stack = 0;
    std::size_t heap = 0;
    std::size_t non_escaping = 0;
    std::size_t escaping = 0;
    std::size_t unknown = 0;
    for (const auto& allocation : report.allocations) {
        allocation.storage == toro::IRStorageClass::Stack ? ++stack : ++heap;
        if (allocation.escape_state == toro::IREscapeState::DoesNotEscape) {
            ++non_escaping;
        } else if (allocation.escape_state == toro::IREscapeState::Escapes) {
            ++escaping;
        } else {
            ++unknown;
        }
    }
    toro::MemoryArcCounts arc;
    for (const auto& callable : report.callables) {
        arc.retains_before += callable.arc.retains_before;
        arc.retains_after += callable.arc.retains_after;
        arc.releases_before += callable.arc.releases_before;
        arc.releases_after += callable.arc.releases_after;
        arc.elided_operations += callable.elided_arc_operations.size();
        arc.ownership_transfers += callable.ownership_transfers.size();
        arc.inferred_borrows += callable.inferred_borrows.size();
    }
    expect(report.summary.managed_allocation_sites
                == report.allocations.size()
            && report.summary.stack_allocation_sites == stack
            && report.summary.heap_allocation_sites == heap,
        "allocation summary did not equal detailed allocation records");
    expect(report.summary.non_escaping_sites == non_escaping
            && report.summary.escaping_sites == escaping
            && report.summary.unknown_escape_sites == unknown,
        "escape summary did not equal detailed allocation records");
    expect(report.summary.arc.retains_before == arc.retains_before
            && report.summary.arc.retains_after == arc.retains_after
            && report.summary.arc.releases_before == arc.releases_before
            && report.summary.arc.releases_after == arc.releases_after
            && report.summary.arc.elided_operations == arc.elided_operations
            && report.summary.arc.ownership_transfers
                == arc.ownership_transfers
            && report.summary.arc.inferred_borrows == arc.inferred_borrows,
        "ARC summary did not equal per-callable records");
}

void expect_final_arc_order_matches(
    const toro::IRProgram& ir, const toro::MemoryReport& report)
{
    for (const auto& schedule : ir.arc_schedules) {
        const auto callable = std::ranges::find_if(report.callables,
            [&](const auto& candidate) {
                return candidate.callable_identity
                    == schedule.callable_identity;
            });
        expect(callable != report.callables.end(),
            "ARC schedule callable was omitted from memory reporting");
        std::size_t active_index = 0;
        for (const auto& operation : schedule.optimized_operations) {
            if (operation.elided
                || operation.kind == toro::IRArcOperationKind::OwnershipTransfer
                || operation.kind == toro::IRArcOperationKind::BorrowedTemporary) {
                continue;
            }
            expect(active_index < callable->final_arc_operations.size(),
                "final ARC report omitted an active operation");
            const auto& reported = callable->final_arc_operations[active_index++];
            expect(reported.kind == operation.kind
                    && reported.value == operation.value
                    && reported.origin.module_name
                        == operation.origin.module_name
                    && reported.origin.location.line
                        == operation.origin.location.line
                    && reported.origin.location.column
                        == operation.origin.location.column,
                "final ARC report changed authoritative schedule order");
        }
        expect(active_index == callable->final_arc_operations.size(),
            "final ARC report added an operation absent from the schedule");
    }
}

void test_empty_and_mixed_allocation_reports()
{
    const auto pure = toro::MemoryReporter().inspect(analyze(
        "function pure(value: int) -> int { return value + 1 }\n"));
    expect(pure.summary.managed_allocation_sites == 0,
        "pure primitive callable reported a managed allocation");
    expect(!pure.callables.empty(),
        "pure primitive callable was omitted from callable reporting");

    const auto ir = analyze(
        "class Box { public function touch() {} }\n"
        "function local() {\n"
        "    first := Box()\n"
        "    second := first\n"
        "    second.touch()\n"
        "}\n"
        "function make() -> Box { return Box() }\n");
    const auto report = toro::MemoryReporter().inspect(ir);
    expect(report.allocations.size() == 2,
        "mixed report did not retain both allocation sites");
    expect(report.summary.stack_allocation_sites == 1
            && report.summary.heap_allocation_sites == 1,
        "mixed report did not distinguish stack and heap storage");
    bool saw_return_reason = false;
    for (const auto& allocation : report.allocations) {
        expect(allocation.origin.location.line != 0,
            "allocation source origin was not retained");
        if (allocation.storage == toro::IRStorageClass::Heap) {
            saw_return_reason = allocation.storage_reason.find("return")
                != std::string::npos;
        }
    }
    expect(saw_return_reason,
        "heap allocation did not retain the analyzer's return reason");
    expect(report.summary.arc.ownership_transfers != 0,
        "ownership transfer was absent from the memory report");
    expect(report.summary.arc.inferred_borrows != 0,
        "inferred borrow was absent from the memory report");
    expect(report.summary.managed_allocation_sites
                == ir.escape_metrics.managed_allocations
            && report.summary.stack_allocation_sites
                == ir.escape_metrics.stack_allocations
            && report.summary.heap_allocation_sites
                == ir.escape_metrics.heap_allocations,
        "memory report disagreed with authoritative escape metrics");
    expect(report.summary.arc.retains_before == ir.arc_metrics.retains_before
            && report.summary.arc.retains_after
                == ir.arc_metrics.retains_after
            && report.summary.arc.releases_before
                == ir.arc_metrics.releases_before
            && report.summary.arc.releases_after
                == ir.arc_metrics.releases_after
            && report.summary.arc.ownership_transfers
                == ir.arc_metrics.ownership_transfers
            && report.summary.arc.inferred_borrows
                == ir.arc_metrics.borrows_inferred,
        "memory report disagreed with authoritative ARC metrics");
    expect_summary_matches_details(report);
    expect_final_arc_order_matches(ir, report);
}

void test_owned_storage_collections_and_interface_borrows()
{
    const auto ir = analyze(
        "interface Named { function label() -> string }\n"
        "class Item implements Named {\n"
        "    public function label() -> string { return \"item\" }\n"
        "}\n"
        "class Base {\n"
        "    public virtual function label() -> string { return \"base\" }\n"
        "}\n"
        "class Derived : Base {\n"
        "    public override function label() -> string { return \"derived\" }\n"
        "}\n"
        "class Holder { public item: Item? }\n"
        "function read(value: Named) -> string { return value.label() }\n"
        "function field_store(holder: Holder) {\n"
        "    value := Item()\n"
        "    holder.item = value\n"
        "}\n"
        "function list_store() {\n"
        "    values := List<Item>()\n"
        "    value := Item()\n"
        "    values.add(value)\n"
        "}\n"
        "function map_store() {\n"
        "    values := Map<string, Item>()\n"
        "    value := Item()\n"
        "    values[\"item\"] = value\n"
        "}\n"
        "function array_store() {\n"
        "    values := Array<Item>(Item())\n"
        "    value := Item()\n"
        "    values[0] = value\n"
        "}\n"
        "function interface_use() {\n"
        "    value := Item()\n"
        "    named: Named = value\n"
        "    print(read(named))\n"
        "}\n"
        "function virtual_use(value: Base) { print(value.label()) }\n"
        "function virtual_setup() {\n"
        "    value := Derived()\n"
        "    virtual_use(value)\n"
        "}\n");
    const auto report = toro::MemoryReporter().inspect(ir);
    std::size_t strong_field_sites = 0;
    std::size_t collection_sites = 0;
    for (const auto& allocation : report.allocations) {
        for (const auto& cause : allocation.escape_causes) {
            if (cause.reason == toro::IREscapeReason::StrongField) {
                ++strong_field_sites;
            } else if (cause.reason
                == toro::IREscapeReason::CollectionStorage) {
                ++collection_sites;
            }
        }
    }
    expect(strong_field_sites == 1,
        "strong class-field ownership was not represented");
    expect(collection_sites >= 3,
        "List/Map/Array managed storage was not represented");
    const auto interface_callable = std::ranges::find_if(report.callables,
        [](const auto& callable) {
            return callable.callable_identity.find("interface_use")
                != std::string::npos;
        });
    expect(interface_callable != report.callables.end()
            && !interface_callable->inferred_borrows.empty(),
        "class-backed interface use did not preserve borrow facts");
    const auto virtual_callable = std::ranges::find_if(report.callables,
        [](const auto& callable) {
            return callable.callable_identity.find("virtual_use")
                != std::string::npos;
        });
    expect(virtual_callable != report.callables.end()
            && !virtual_callable->inferred_borrows.empty(),
        "virtual-call memory behavior did not preserve borrow facts");
    expect_summary_matches_details(report);
}

void test_weak_fields_and_cycles()
{
    const auto weak = toro::MemoryReporter().inspect(analyze(
        "class Item {}\n"
        "class Watcher { public weak observed: Item? }\n"
        "function watch(watcher: Watcher) {\n"
        "    value := Item()\n"
        "    watcher.observed = value\n"
        "}\n"));
    expect(weak.summary.weak_fields == 1 && weak.weak_fields.size() == 1,
        "weak field was not reported as non-owning storage");
    expect(weak.summary.possible_strong_cycles == 0,
        "weak field incorrectly contributed a possible strong cycle");
    const auto observed = std::ranges::find_if(weak.allocations,
        [](const auto& allocation) {
            return allocation.storage_reason.find("weak target")
                != std::string::npos;
        });
    expect(observed != weak.allocations.end()
            && observed->storage == toro::IRStorageClass::Heap,
        "weak-observed target did not retain its heap fallback reason");
    expect(toro::MemoryReporter().format(weak).find("; non-owning")
            != std::string::npos,
        "formatted weak field did not identify non-owning storage");

    const auto cycle = toro::MemoryReporter().inspect(analyze(
        "class First { public second: Second? }\n"
        "class Second { public first: First? }\n"));
    expect(cycle.summary.possible_strong_cycles == 1
            && cycle.cycles.size() == 1
            && cycle.cycles.front().edges.size() == 2,
        "possible strong cycle was not consumed from analyzer results");
    expect(toro::MemoryReporter().format(cycle).find("cycle 1 advisory")
            != std::string::npos,
        "formatted cycle did not retain advisory wording");
}

void test_generics_determinism_and_non_mutation()
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

    const auto first = toro::MemoryReporter().report(ir);
    const auto second = toro::MemoryReporter().report(ir);
    expect(first == second, "memory report output was not deterministic");
    expect(first.find("Box<int>") != std::string::npos,
        "concrete generic identity was not preserved");
    expect(first.find("scope static-compiler-analysis") != std::string::npos,
        "memory report did not distinguish static facts from profiling");
    expect(effects_before == toro::dump_ir(ir, true, false, false, false)
            && ownership_before
                == toro::dump_ir(ir, false, false, false, true)
            && escape_before == toro::dump_ir(ir, false, false, true, false)
            && arc_before == toro::dump_ir(ir, false, true, false, false),
        "memory reporting mutated an authoritative analysis view");
    expect(effect_count == ir.effect_summaries.size()
            && ownership_count == ir.ownership_summaries.size()
            && cycle_count == ir.strong_reference_cycles.size()
            && allocation_count == ir.escape_allocations.size()
            && arc_schedule_count == ir.arc_schedules.size()
            && performance_count == ir.performance_contract_results.size(),
        "memory reporting changed authoritative analysis results");
    expect(generated_before == toro::CGenerator().generate(ir),
        "memory reporting changed generated C");
}

} // namespace

int main()
{
    try {
        test_empty_and_mixed_allocation_reports();
        test_owned_storage_collections_and_interface_borrows();
        test_weak_fields_and_cycles();
        test_generics_determinism_and_non_mutation();
    } catch (const std::exception& error) {
        std::cerr << "memory reporter test failure: " << error.what() << '\n';
        return 1;
    }
    std::cout << "memory reporter tests passed\n";
    return 0;
}
