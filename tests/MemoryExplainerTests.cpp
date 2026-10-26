#include "toro/ArcOptimizer.hpp"
#include "toro/BorrowInferenceAnalyzer.hpp"
#include "toro/CGenerator.hpp"
#include "toro/EffectAnalyzer.hpp"
#include "toro/EscapeAnalyzer.hpp"
#include "toro/IRLowerer.hpp"
#include "toro/Lexer.hpp"
#include "toro/MemoryExplainer.hpp"
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

bool has_kind(const toro::MemoryExplanationResult& result,
    toro::MemoryExplanationKind kind)
{
    return std::ranges::any_of(result.explanations,
        [&](const auto& explanation) { return explanation.kind == kind; });
}

bool has_reason(const toro::MemoryExplanationResult& result,
    std::string_view code)
{
    return std::ranges::any_of(result.explanations,
        [&](const auto& explanation) {
            return std::ranges::any_of(explanation.reasons,
                [&](const auto& reason) { return reason.code == code; });
        });
}

toro::MemoryExplanationResult explain_origin(const toro::IRProgram& ir,
    const toro::MemoryReport& memory, const toro::IROrigin& origin)
{
    return toro::MemoryExplainer().explain(ir, memory, origin.module_name,
        origin.location.line, origin.location.column);
}

void test_stack_heap_and_escape_explanations()
{
    const auto ir = analyze(
        "class Box {}\n"
        "function local() {\n"
        "    value := Box()\n"
        "}\n"
        "function make() -> Box {\n"
        "    return Box()\n"
        "}\n");
    const auto memory = toro::MemoryReporter().inspect(ir);
    const auto stack = std::ranges::find_if(memory.allocations,
        [](const auto& allocation) {
            return allocation.storage == toro::IRStorageClass::Stack;
        });
    const auto heap = std::ranges::find_if(memory.allocations,
        [](const auto& allocation) {
            return allocation.storage == toro::IRStorageClass::Heap;
        });
    expect(stack != memory.allocations.end()
            && heap != memory.allocations.end(),
        "fixture did not produce stack and heap allocations");

    const auto stack_result = explain_origin(ir, memory, stack->origin);
    expect(stack_result.status == toro::MemoryExplanationStatus::Explained
            && has_kind(stack_result, toro::MemoryExplanationKind::Allocation),
        "stack allocation was not explained");
    expect(std::ranges::any_of(stack_result.explanations,
               [](const auto& explanation) {
                   return explanation.decision == "stack-storage";
               }),
        "stack storage decision was not preserved");

    const auto heap_result = explain_origin(ir, memory, heap->origin);
    expect(std::ranges::any_of(heap_result.explanations,
               [](const auto& explanation) {
                   return explanation.decision == "heap-storage";
               })
            && has_reason(heap_result, "return"),
        "return-escaping heap decision was not explained");

    const auto return_cause = std::ranges::find_if(heap->escape_causes,
        [](const auto& cause) {
            return cause.reason == toro::IREscapeReason::Return;
        });
    expect(return_cause != heap->escape_causes.end(),
        "return escape cause was absent");
    const auto escape_result = explain_origin(ir, memory, return_cause->origin);
    expect(has_kind(escape_result, toro::MemoryExplanationKind::Escape)
            && has_reason(escape_result, "return"),
        "escape-producing return operation was not explained");
}

void test_field_collection_weak_and_cycle_explanations()
{
    const auto ir = analyze(
        "class Item {}\n"
        "class Holder { public item: Item? }\n"
        "class Watcher { public weak observed: Item? }\n"
        "class First { public second: Second? }\n"
        "class Second { public first: First? }\n"
        "function store(holder: Holder, watcher: Watcher) {\n"
        "    field_value := Item()\n"
        "    holder.item = field_value\n"
        "    values := List<Item>()\n"
        "    list_value := Item()\n"
        "    values.add(list_value)\n"
        "    weak_value := Item()\n"
        "    watcher.observed = weak_value\n"
        "}\n");
    const auto memory = toro::MemoryReporter().inspect(ir);
    for (const auto reason : {toro::IREscapeReason::StrongField,
             toro::IREscapeReason::CollectionStorage,
             toro::IREscapeReason::WeakObservation}) {
        const toro::IREscapeCause* matching = nullptr;
        for (const auto& allocation : memory.allocations) {
            const auto cause = std::ranges::find_if(allocation.escape_causes,
                [&](const auto& candidate) {
                    return candidate.reason == reason;
                });
            if (cause != allocation.escape_causes.end()) {
                matching = &*cause;
                break;
            }
        }
        expect(matching != nullptr, "expected escape cause was absent");
        const auto result = explain_origin(ir, memory, matching->origin);
        expect(has_kind(result, toro::MemoryExplanationKind::Escape)
                && has_reason(result, toro::ir_escape_reason_name(reason)),
            "field, collection, or weak escape was not explained");
    }

    const auto weak_declaration = std::ranges::find_if(ir.declarations,
        [](const auto& declaration) { return declaration.name == "Watcher"; });
    expect(weak_declaration != ir.declarations.end(),
        "weak owner declaration was absent");
    const auto weak_field = std::ranges::find_if(weak_declaration->fields,
        [](const auto& field) { return field.name == "observed"; });
    const auto weak_result = explain_origin(ir, memory, weak_field->origin);
    expect(has_kind(weak_result, toro::MemoryExplanationKind::WeakField)
            && has_reason(weak_result, "explicit-weak-declaration"),
        "weak field declaration was not explained distinctly");

    expect(!memory.cycles.empty(), "cycle fixture did not produce a cycle");
    const auto cycle_result = explain_origin(
        ir, memory, memory.cycles.front().edges.front().origin);
    expect(has_kind(cycle_result,
               toro::MemoryExplanationKind::PossibleStrongCycle)
            && has_kind(cycle_result,
                toro::MemoryExplanationKind::StrongField),
        "strong field cycle advisory was not explained");
}

void test_arc_borrow_transfer_and_dynamic_explanations()
{
    const auto ir = analyze(
        "interface Named { function label() -> string }\n"
        "class Item implements Named {\n"
        "    public function label() -> string { return \"item\" }\n"
        "}\n"
        "class Base { public virtual function label() -> string { return \"base\" } }\n"
        "class Derived : Base { public override function label() -> string { return \"derived\" } }\n"
        "function observe(value: Item) { print(value.label()) }\n"
        "function make() -> Item { return Item() }\n"
        "function preserve(value: Item) -> Item { return value }\n"
        "function use_named(value: Named) { print(value.label()) }\n"
        "function use_base(value: Base) { print(value.label()) }\n"
        "function work() {\n"
        "    value := Item()\n"
        "    observe(value)\n"
        "    named: Named = value\n"
        "    use_named(named)\n"
        "    derived := Derived()\n"
        "    use_base(derived)\n"
        "}\n");
    const auto memory = toro::MemoryReporter().inspect(ir);

    bool saw_borrow = false;
    bool saw_transfer = false;
    bool saw_elision = false;
    bool saw_kept = false;
    bool saw_interface_target = false;
    bool saw_virtual_target = false;
    for (const auto& callable : memory.callables) {
        for (const auto& operation : callable.inferred_borrows) {
            const auto result = explain_origin(ir, memory, operation.origin);
            saw_borrow = saw_borrow
                || has_kind(result, toro::MemoryExplanationKind::Borrow);
            saw_interface_target = saw_interface_target
                || has_reason(result, "reachable-interface-target");
            saw_virtual_target = saw_virtual_target
                || has_reason(result, "reachable-virtual-target");
        }
        for (const auto& operation : callable.ownership_transfers) {
            const auto result = explain_origin(ir, memory, operation.origin);
            saw_transfer = saw_transfer
                || has_kind(result, toro::MemoryExplanationKind::Transfer);
        }
        for (const auto& operation : callable.elided_arc_operations) {
            const auto result = explain_origin(ir, memory, operation.origin);
            saw_elision = saw_elision
                || (has_kind(result, toro::MemoryExplanationKind::ArcElided)
                    && (has_reason(result, "inferred-borrow")
                        || has_reason(result, "ownership-transfer")));
        }
        for (const auto& operation : callable.final_arc_operations) {
            if (operation.kind != toro::IRArcOperationKind::Retain
                && operation.kind != toro::IRArcOperationKind::Release) {
                continue;
            }
            const auto result = explain_origin(ir, memory, operation.origin);
            saw_kept = saw_kept
                || (has_kind(result, toro::MemoryExplanationKind::ArcKept)
                    && has_reason(result, "arc-retention"));
        }
    }
    expect(saw_borrow, "inferred borrow was not explained");
    expect(saw_transfer, "ownership transfer was not explained");
    expect(saw_elision, "ARC elision provenance was not explained");
    expect(saw_kept, "retained ARC operation was not explained");
    expect(saw_interface_target && saw_virtual_target,
        "dynamic borrow explanation omitted interface or virtual targets");
}

void test_generic_identity_ambiguity_and_no_result()
{
    const auto ir = analyze(
        "class Box<T> { public value: T }\n"
        "function work() {\n"
        "    box := Box<int>(value: 1)\n"
        "    print(box.value)\n"
        "}\n");
    const auto memory = toro::MemoryReporter().inspect(ir);
    expect(!memory.allocations.empty(), "generic allocation was absent");
    const auto generic = explain_origin(
        ir, memory, memory.allocations.front().origin);
    expect(std::ranges::any_of(generic.explanations,
               [](const auto& explanation) {
                   return explanation.type
                       && toro::type_name(explanation.type->type) == "Box<int>";
               }),
        "concrete generic specialization identity was not preserved");

    auto foreign_memory = memory;
    foreign_memory.allocations.front().escape_causes.push_back(
        toro::IREscapeCause{
            toro::IREscapeReason::ForeignCall,
            toro::IROrigin{memory.allocations.front().origin.module_name,
                toro::SourceLocation{40, 9}},
            "unknown foreign ownership behavior", true, true,
        });
    const auto foreign = toro::MemoryExplainer().explain(ir, foreign_memory,
        memory.allocations.front().origin.module_name, 40, 9);
    expect(has_kind(foreign, toro::MemoryExplanationKind::Escape)
            && has_reason(foreign, "foreign-call")
            && std::ranges::any_of(foreign.explanations,
                [](const auto& explanation) {
                    return explanation.decision == "escape-unknown";
                }),
        "unknown foreign escape provenance was not explained");

    toro::MemoryReport synthetic;
    toro::CallableMemoryReport callable;
    callable.callable_identity = "app::work";
    callable.final_arc_operations.push_back(toro::MemoryArcOperationReport{
        toro::IRArcOperationKind::Retain, "first",
        toro::IROrigin{"app", toro::SourceLocation{8, 5}}, "first retain",
        std::nullopt, std::nullopt});
    callable.final_arc_operations.push_back(toro::MemoryArcOperationReport{
        toro::IRArcOperationKind::Release, "second",
        toro::IROrigin{"app", toro::SourceLocation{8, 17}}, "second release",
        std::nullopt, std::nullopt});
    synthetic.callables.push_back(std::move(callable));
    const toro::IRProgram empty_ir;
    const auto ambiguous = toro::MemoryExplainer().explain(
        empty_ir, synthetic, "app", 8);
    expect(ambiguous.status == toro::MemoryExplanationStatus::Ambiguous
            && ambiguous.candidates.size() == 2
            && ambiguous.candidates[0].origin.location.column == 5
            && ambiguous.candidates[1].origin.location.column == 17,
        "line-only ambiguity was not deterministic");
    const auto exact = toro::MemoryExplainer().explain(
        empty_ir, synthetic, "app", 8, 17);
    expect(exact.status == toro::MemoryExplanationStatus::Explained
            && exact.explanations.size() == 1
            && exact.explanations.front().subject_identity.find("second")
                != std::string::npos,
        "exact line-column target did not resolve one source location");
    const auto none = toro::MemoryExplainer().explain(
        empty_ir, synthetic, "app", 9);
    expect(none.status == toro::MemoryExplanationStatus::NoMemoryDecision,
        "no-result location did not report no memory decision");
    expect(toro::MemoryExplainer().format(ambiguous)
            == toro::MemoryExplainer().format(ambiguous),
        "ambiguity formatting was not deterministic");
    expect(toro::MemoryExplainer().format(none).find("no memory decision")
            != std::string::npos,
        "no-result formatting was unclear");
}

void test_read_only_and_generated_c_stability()
{
    auto ir = analyze(
        "class Box {}\n"
        "function work() { value := Box() }\n");
    const auto effects = toro::dump_ir(ir, true, false, false, false);
    const auto ownership = toro::dump_ir(ir, false, false, false, true);
    const auto escape = toro::dump_ir(ir, false, false, true, false);
    const auto arc = toro::dump_ir(ir, false, true, false, false);
    const auto generated = toro::CGenerator().generate(ir);
    const auto memory = toro::MemoryReporter().inspect(ir);
    const auto first = explain_origin(
        ir, memory, memory.allocations.front().origin);
    const auto second = explain_origin(
        ir, memory, memory.allocations.front().origin);
    expect(toro::MemoryExplainer().format(first)
            == toro::MemoryExplainer().format(second),
        "repeated memory explanations differed");
    expect(effects == toro::dump_ir(ir, true, false, false, false)
            && ownership == toro::dump_ir(ir, false, false, false, true)
            && escape == toro::dump_ir(ir, false, false, true, false)
            && arc == toro::dump_ir(ir, false, true, false, false),
        "memory explanation mutated an authoritative analysis view");
    expect(generated == toro::CGenerator().generate(ir),
        "memory explanation changed generated C");
}

} // namespace

int main()
{
    try {
        test_stack_heap_and_escape_explanations();
        test_field_collection_weak_and_cycle_explanations();
        test_arc_borrow_transfer_and_dynamic_explanations();
        test_generic_identity_ambiguity_and_no_result();
        test_read_only_and_generated_c_stability();
    } catch (const std::exception& error) {
        std::cerr << "memory explainer test failure: " << error.what() << '\n';
        return 1;
    }
    std::cout << "memory explainer tests passed\n";
    return 0;
}
