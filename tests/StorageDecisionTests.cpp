#include "toro/ArcOptimizer.hpp"
#include "toro/BorrowInferenceAnalyzer.hpp"
#include "toro/CGenerator.hpp"
#include "toro/EffectAnalyzer.hpp"
#include "toro/EscapeAnalyzer.hpp"
#include "toro/IRLowerer.hpp"
#include "toro/Lexer.hpp"
#include "toro/OwnershipSummaryAnalyzer.hpp"
#include "toro/Parser.hpp"
#include "toro/SemanticAnalyzer.hpp"
#include "toro/StorageDecisionAnalyzer.hpp"
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

toro::IRProgram analyze(std::string_view source, bool enable_stack = true)
{
    auto program = toro::Parser(toro::Lexer(source).tokenize()).parse_program();
    toro::SemanticAnalyzer().analyze(program);
    toro::TypeChecker().check(program);
    auto ir = toro::IRLowerer().lower(program);
    toro::EffectAnalyzer().analyze(ir);
    toro::OwnershipSummaryAnalyzer().analyze(ir);
    toro::EscapeAnalyzer().analyze(ir);
    toro::StorageDecisionAnalyzer().analyze(ir, enable_stack);
    return ir;
}

const toro::IRAllocationEscape& allocation(
    const toro::IRProgram& program,
    std::string_view callable)
{
    for (const auto& candidate : program.escape_allocations) {
        if (candidate.callable_identity.ends_with(callable)) return candidate;
    }
    throw std::runtime_error("missing allocation");
}

std::string generate(toro::IRProgram ir)
{
    toro::ArcMaterializer().materialize(ir);
    toro::OwnershipTransferAnalyzer().analyze(ir);
    toro::BorrowInferenceAnalyzer().analyze(ir);
    toro::ArcOptimizer().optimize(ir);
    return toro::CGenerator().generate(ir);
}

void test_stack_promotion_and_heap_only_mode()
{
    constexpr std::string_view source =
        "class Item {\n"
        "    public value: int\n"
        "    destroy() { print(\"destroyed\") }\n"
        "}\n"
        "function main() {\n"
        "    item := Item(value: 1)\n"
        "    alias := item\n"
        "    alias.value = 2\n"
        "    print(item.value)\n"
        "}\n";
    const auto optimized = analyze(source);
    const auto heap_only = analyze(source, false);
    expect(allocation(optimized, "main()").storage
            == toro::IRStorageClass::Stack,
        "local class was not stack promoted");
    expect(allocation(heap_only, "main()").storage
            == toro::IRStorageClass::Heap,
        "heap-only mode did not disable promotion");
    expect(optimized.escape_metrics.stack_allocations == 1
            && optimized.escape_metrics.heap_allocations == 0,
        "stack metrics were incorrect");
    const auto c = generate(optimized);
    expect(c.find("toro_new_class_0_storage") != std::string::npos,
        "automatic class storage was not emitted");
    expect(c.find("(toro_new_class_0)->toro_heap_allocated = false")
            != std::string::npos,
        "stack storage marker was not emitted");
    expect(c.find("toro_new_class_0 = malloc") == std::string::npos,
        "promoted allocation still used the heap allocator");
    expect(c.find("if (toro_heap_allocated) { free(toro_value); }")
            != std::string::npos,
        "shared finalization did not guard heap-only object deallocation");
    auto stack_arc = optimized;
    auto heap_arc = heap_only;
    toro::ArcMaterializer().materialize(stack_arc);
    toro::OwnershipTransferAnalyzer().analyze(stack_arc);
    toro::BorrowInferenceAnalyzer().analyze(stack_arc, false);
    toro::ArcOptimizer().optimize(stack_arc);
    toro::ArcMaterializer().materialize(heap_arc);
    toro::OwnershipTransferAnalyzer().analyze(heap_arc);
    toro::BorrowInferenceAnalyzer().analyze(heap_arc, false);
    toro::ArcOptimizer().optimize(heap_arc);
    expect(stack_arc.arc_metrics.retains_after
                < heap_arc.arc_metrics.retains_after
            && stack_arc.arc_metrics.releases_after
                < heap_arc.arc_metrics.releases_after,
        "stack promotion did not remove local ARC traffic");
}

void test_supported_local_shapes()
{
    const auto ir = analyze(
        "interface Named { function name() -> string }\n"
        "class Base {\n"
        "    public virtual function name() -> string { return \"base\" }\n"
        "}\n"
        "class Box<T> : Base implements Named {\n"
        "    public value: T\n"
        "    init(value: T) { self.value = value }\n"
        "    public override function name() -> string { return \"box\" }\n"
        "}\n"
        "function shapes(flag: bool) {\n"
        "    box := Box<int>(1)\n"
        "    base: Base = box\n"
        "    named: Named = box\n"
        "    print(base.name())\n"
        "    print(named.name())\n"
        "    nullable: Box<int>? = box\n"
        "    if flag {\n"
        "        local := Box<string>(\"branch\")\n"
        "        print(local.value)\n"
        "    }\n"
        "    while flag {\n"
        "        local := Box<int>(2)\n"
        "        print(local.value)\n"
        "        stop\n"
        "    }\n"
        "}\n");
    expect(ir.escape_metrics.managed_allocations == 3
            && ir.escape_metrics.stack_allocations == 3,
        "safe generic/inheritance/interface/control-flow shapes were not promoted");
}

void test_strong_field_cleanup_and_heap_fallbacks()
{
    const auto ir = analyze(
        "extern \"C\" function observe(value: c_int) -> c_int\n"
        "class Child {}\n"
        "class Parent { public child: Child? }\n"
        "class Watcher { public weak child: Child? }\n"
        "global_child := Child()\n"
        "function returned() -> Child { return Child() }\n"
        "function local_parent() {\n"
        "    child := Child()\n"
        "    parent := Parent(child: child)\n"
        "    print(parent.child != null)\n"
        "}\n"
        "function weak_target(watcher: Watcher) {\n"
        "    child := Child()\n"
        "    watcher.child = child\n"
        "}\n"
        "function field_escape(parent: Parent) {\n"
        "    child := Child()\n"
        "    parent.child = child\n"
        "}\n"
        "function collection() {\n"
        "    child := Child()\n"
        "    values := List<Child>()\n"
        "    values.add(child)\n"
        "}\n");
    expect(ir.escape_metrics.stack_allocations == 1,
        "strong-field owner was not the sole promoted allocation");
    expect(allocation(ir, "returned()").storage == toro::IRStorageClass::Heap,
        "returned allocation was promoted");
    expect(allocation(ir, "weak_target(Watcher)").storage
            == toro::IRStorageClass::Heap,
        "weak-observed target was promoted");
    expect(allocation(ir, "field_escape(Parent)").storage
            == toro::IRStorageClass::Heap,
        "strong-field escaping target was promoted");
    expect(allocation(ir, "collection()").storage == toro::IRStorageClass::Heap,
        "collection-stored target was promoted");
}

void test_uncertain_dynamic_dispatch_falls_back()
{
    const auto ir = analyze(
        "class Sink { public value: Base? }\n"
        "class Base {\n"
        "    public virtual function store(sink: Sink) { }\n"
        "}\n"
        "class Derived : Base {\n"
        "    public override function store(sink: Sink) { sink.value = self }\n"
        "}\n"
        "function uncertain(sink: Sink) {\n"
        "    value := Derived()\n"
        "    value.store(sink)\n"
        "}\n");
    const auto& value = allocation(ir, "uncertain(Sink)");
    expect(value.state == toro::IREscapeState::Unknown
            && value.storage == toro::IRStorageClass::Heap,
        "escaping dynamic dispatch did not use conservative heap fallback");
}

void test_foreign_barrier_and_reassignment_fall_back()
{
    const auto ir = analyze(
        "extern \"C\" function puts(text: c_string) -> c_int\n"
        "class Item { public marker: int }\n"
        "function foreign_barrier() {\n"
        "    value := Item()\n"
        "    puts(\"foreign\" as c_string)\n"
        "    print(value.marker)\n"
        "}\n"
        "function reassigned(flag: bool) {\n"
        "    value := Item()\n"
        "    other := Item()\n"
        "    if flag { value = other }\n"
        "    print(value.marker)\n"
        "}\n");
    const auto& foreign = allocation(ir, "foreign_barrier()");
    expect(foreign.state == toro::IREscapeState::Unknown
            && foreign.storage == toro::IRStorageClass::Heap,
        "foreign uncertainty did not force heap storage");
    for (const auto& candidate : ir.escape_allocations) {
        if (!candidate.callable_identity.ends_with("reassigned(bool)")) continue;
        expect(candidate.storage == toro::IRStorageClass::Heap,
            "managed reassignment did not force stable heap ownership");
    }
}

void test_dump_determinism()
{
    constexpr std::string_view source =
        "class Item { public marker: int }\n"
        "function main() { value := Item() }\n";
    const auto first = analyze(source);
    const auto second = analyze(source);
    expect(toro::dump_ir(first, false, false, true)
            == toro::dump_ir(second, false, false, true),
        "storage inspection was not deterministic");
}

void test_storage_decisions_do_not_change_effect_output()
{
    auto program = toro::Parser(toro::Lexer(
        "class Item {}\n"
        "function main() {\n"
        "    value := Item()\n"
        "    print(\"item\")\n"
        "}\n")
            .tokenize()).parse_program();
    toro::SemanticAnalyzer().analyze(program);
    toro::TypeChecker().check(program);
    auto ir = toro::IRLowerer().lower(program);
    toro::EffectAnalyzer().analyze(ir);
    toro::OwnershipSummaryAnalyzer().analyze(ir);
    const auto before = toro::dump_ir(ir, true, false, false);
    toro::EscapeAnalyzer().analyze(ir);
    toro::StorageDecisionAnalyzer().analyze(ir);
    expect(before == toro::dump_ir(ir, true, false, false),
        "storage decisions changed effect-analysis output");
}

void test_source_arena_region_and_fallbacks()
{
    const auto ir = analyze(
        "class Item {\n"
        "    public value: int\n"
        "    init(value: int) { self.value = value }\n"
        "    destroy() { print(self.value) }\n"
        "}\n"
        "function work(limit: int) {\n"
        "    values := List<Item>()\n"
        "    index := 0\n"
        "    while index < limit {\n"
        "      arena {\n"
        "        first := Item(index)\n"
        "        alias := first\n"
        "        print(alias.value)\n"
        "        values.add(first)\n"
        "        second := Item(index + 1)\n"
        "        values.add(second)\n"
        "        values.clear()\n"
        "        index = index + 1\n"
        "      }\n"
        "    }\n"
        "}\n");
    expect(ir.escape_metrics.arena_allocations == 2
            && ir.escape_metrics.heap_allocations == 0
            && ir.regions.size() == 1,
        "bounded List-owned objects were not assigned to one arena region");
    expect(ir.regions.front().capacity_bytes == 0
            && ir.regions.front().max_objects == 0
            && ir.regions.front().teardown_entries == 2
            && ir.regions.front().teardown_order.size() == 2,
        "production arena teardown metadata was not retained");
    for (const auto& candidate : ir.escape_allocations) {
        expect(candidate.storage == toro::IRStorageClass::Arena
                && !candidate.region_identity.empty(),
            "arena allocation lacked storage or provenance");
    }
    const auto dump = toro::dump_ir(ir, false, false, true);
    expect(dump.find("storage arena") != std::string::npos
            && dump.find("region=<direct>.arena@") != std::string::npos,
        "arena decision was absent from IR inspection");

    const auto fallback = analyze(
        "class Item {}\n"
        "class Watcher { public weak item: Item? }\n"
        "function work(watcher: Watcher, limit: int) {\n"
        "    values := List<Item>()\n"
        "    index := 0\n"
        "    while index < limit {\n"
        "      arena {\n"
        "        item := Item()\n"
        "        ordinary := Item()\n"
        "        watcher.item = item\n"
        "        values.add(item)\n"
        "        values.add(ordinary)\n"
        "        values.clear()\n"
        "        index = index + 1\n"
        "      }\n"
        "    }\n"
        "}\n");
    expect(fallback.escape_metrics.arena_allocations == 0
            && fallback.escape_metrics.heap_allocations == 2
            && fallback.escape_metrics.arena_fallbacks == 2
            && fallback.regions.front().runtime_arc_suppressions == 0,
        "weak-observed candidate did not conservatively fall back to heap");

    const auto persistent = analyze(
        "class Item {}\n"
        "function work(limit: int) {\n"
        "    transient := List<Item>()\n"
        "    persistent := List<Item>()\n"
        "    index := 0\n"
        "    while index < limit {\n"
        "      arena {\n"
        "        item := Item()\n"
        "        transient.add(item)\n"
        "        persistent.add(item)\n"
        "        transient.clear()\n"
        "        index = index + 1\n"
        "      }\n"
        "    }\n"
        "}\n");
    expect(persistent.escape_metrics.arena_allocations == 0
            && persistent.escape_metrics.arena_fallbacks == 1,
        "candidate stored in an uncleared collection did not fall back");

    const auto cloned = analyze(
        "class Item {}\n"
        "function work() {\n"
        "  arena {\n"
        "    values := List<Item>()\n"
        "    item := Item()\n"
        "    values.add(item)\n"
        "    copy := values.clone()\n"
        "    values.clear()\n"
        "    copy.clear()\n"
        "  }\n"
        "}\n");
    expect(cloned.escape_metrics.arena_allocations == 0
            && cloned.escape_allocations.front().storage_reason.find("cloned")
                != std::string::npos,
        "candidate reachable through a cloned collection did not fall back");

    const auto strong_graph = analyze(
        "class Item {}\n"
        "class Owner {\n"
        "  public child: Item\n"
        "  init(child: Item) { self.child = child }\n"
        "}\n"
        "function work() {\n"
        "  arena {\n"
        "    owners := List<Owner>()\n"
        "    child := Item()\n"
        "    owner := Owner(child)\n"
        "    owners.add(owner)\n"
        "    owners.clear()\n"
        "  }\n"
        "}\n");
    expect(strong_graph.escape_metrics.arena_allocations == 2
            && strong_graph.escape_metrics.stack_allocations == 0,
        "same-region strong owner and child were not both arena-backed");

    const auto incompatible_nested = analyze(
        "class Item {}\n"
        "class Owner { public child: Item? }\n"
        "function work() {\n"
        "  arena {\n"
        "    owners := List<Owner>()\n"
        "    owner := Owner()\n"
        "    owners.add(owner)\n"
        "    arena {\n"
        "      child := Item()\n"
        "      owner.child = child\n"
        "    }\n"
        "    owners.clear()\n"
        "  }\n"
        "}\n");
    expect(incompatible_nested.regions.size() == 2
            && incompatible_nested.escape_metrics.arena_allocations == 1
            && incompatible_nested.escape_metrics.heap_allocations == 1,
        "outer owner retained incompatible inner arena storage");
    expect(incompatible_nested.regions.back().parent_identity
            == incompatible_nested.regions.front().identity,
        "nested arena parent identity was not recorded");

    const auto early_exit = analyze(
        "class Item {}\n"
        "function work(exit: bool) {\n"
        "  arena {\n"
        "    values := List<Item>()\n"
        "    item := Item()\n"
        "    values.add(item)\n"
        "    if exit { return }\n"
        "    values.clear()\n"
        "  }\n"
        "}\n");
    expect(early_exit.escape_metrics.arena_allocations == 0
            && early_exit.escape_allocations.front().storage_reason.find(
                "early exit") != std::string::npos,
        "collection clear bypassed by an early exit did not fall back");

    const auto foreign = analyze(
        "extern \"C\" function puts(text: c_string) -> c_int\n"
        "class Item { public marker: int }\n"
        "class Owner {\n"
        "  public item: Item\n"
        "  init(item: Item) { self.item = item }\n"
        "}\n"
        "function work() {\n"
        "  arena {\n"
        "    item := Item()\n"
        "    owner := Owner(item)\n"
        "    puts(\"foreign\" as c_string)\n"
        "    print(owner.item.marker)\n"
        "  }\n"
        "}\n");
    expect(foreign.escape_metrics.arena_allocations == 0
            && foreign.escape_metrics.heap_allocations
                + foreign.escape_metrics.stack_allocations == 2,
        "foreign-call uncertainty allowed arena storage");

    const auto generic = analyze(
        "class Box<T> {\n"
        "  public value: T\n"
        "  init(value: T) { self.value = value }\n"
        "}\n"
        "function work() {\n"
        "  arena {\n"
        "    values := List<Box<int>>()\n"
        "    box := Box<int>(10)\n"
        "    values.add(box)\n"
        "    values.clear()\n"
        "  }\n"
        "}\n");
    expect(generic.escape_metrics.arena_allocations == 1
            && generic.escape_allocations.front().type.type.arguments.size() == 1,
        "concrete generic arena eligibility lost its substitution identity");

    const auto transported = analyze(
        "class Item {}\n"
        "enum Payload { item(Item) }\n"
        "function result_value() -> Result<Item, string> {\n"
        "  arena {\n"
        "    item := Item()\n"
        "    return ok(item)\n"
        "  }\n"
        "}\n"
        "function enum_value() -> Payload {\n"
        "  arena {\n"
        "    item := Item()\n"
        "    return Payload::item(item)\n"
        "  }\n"
        "}\n");
    expect(transported.escape_metrics.arena_allocations == 0
            && transported.escape_metrics.heap_allocations == 2,
        "Result/enum transport beyond an arena did not preserve heap fallback: heap="
            + std::to_string(transported.escape_metrics.heap_allocations)
            + " stack=" + std::to_string(transported.escape_metrics.stack_allocations)
            + " arena=" + std::to_string(transported.escape_metrics.arena_allocations));
}

} // namespace

int main()
{
    try {
        test_stack_promotion_and_heap_only_mode();
        test_supported_local_shapes();
        test_strong_field_cleanup_and_heap_fallbacks();
        test_uncertain_dynamic_dispatch_falls_back();
        test_foreign_barrier_and_reassignment_fall_back();
        test_dump_determinism();
        test_storage_decisions_do_not_change_effect_output();
        test_source_arena_region_and_fallbacks();
    } catch (const std::exception& error) {
        std::cerr << "storage decision test failure: " << error.what() << '\n';
        return 1;
    }
    std::cout << "storage decision tests passed\n";
    return 0;
}
