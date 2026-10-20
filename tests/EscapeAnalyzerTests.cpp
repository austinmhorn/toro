#include "toro/ArcOptimizer.hpp"
#include "toro/CGenerator.hpp"
#include "toro/EffectAnalyzer.hpp"
#include "toro/EscapeAnalyzer.hpp"
#include "toro/IRLowerer.hpp"
#include "toro/Lexer.hpp"
#include "toro/Parser.hpp"
#include "toro/SemanticAnalyzer.hpp"
#include "toro/TypeChecker.hpp"

#include <exception>
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
    toro::EscapeAnalyzer().analyze(ir);
    return ir;
}

const toro::IRAllocationEscape& allocation(
    const toro::IRProgram& program,
    std::string_view callable_suffix)
{
    for (const auto& candidate : program.escape_allocations) {
        if (candidate.callable_identity.ends_with(callable_suffix)) {
            return candidate;
        }
    }
    throw std::runtime_error("missing managed allocation");
}

bool has_reason(
    const toro::IRAllocationEscape& allocation,
    toro::IREscapeReason reason,
    bool causes_escape = true)
{
    for (const auto& cause : allocation.causes) {
        if (cause.reason == reason && cause.causes_escape == causes_escape) {
            return true;
        }
    }
    return false;
}

void test_local_aliases_do_not_escape()
{
    const auto ir = analyze(
        "class Item { public name: string }\n"
        "function local() {\n"
        "    first := Item(name: \"one\")\n"
        "    second := first\n"
        "    print(second.name)\n"
        "}\n");
    const auto& value = allocation(ir, "local()");
    expect(value.state == toro::IREscapeState::DoesNotEscape,
        "local alias was classified as escaping");
    expect(ir.escape_metrics.managed_allocations == 1
            && ir.escape_metrics.non_escaping == 1,
        "local allocation metrics were incorrect");
}

void test_return_strong_field_and_weak_observation()
{
    const auto ir = analyze(
        "class Item {}\n"
        "class World {\n"
        "    public item: Item?\n"
        "    public weak observed: Item?\n"
        "}\n"
        "function returned() -> Item {\n"
        "    value := Item()\n"
        "    return value\n"
        "}\n"
        "function stored(world: World) {\n"
        "    value := Item()\n"
        "    world.item = value\n"
        "}\n"
        "function watched(world: World) {\n"
        "    value := Item()\n"
        "    world.observed = value\n"
        "}\n");
    const auto& returned = allocation(ir, "returned()");
    expect(returned.state == toro::IREscapeState::Escapes
            && has_reason(returned, toro::IREscapeReason::Return),
        "returned allocation did not escape through return");
    const auto& stored = allocation(ir, "stored(World)");
    expect(stored.state == toro::IREscapeState::Escapes
            && has_reason(stored, toro::IREscapeReason::StrongField),
        "strong-field allocation did not escape");
    const auto& watched = allocation(ir, "watched(World)");
    expect(watched.state == toro::IREscapeState::DoesNotEscape
            && has_reason(
                watched, toro::IREscapeReason::WeakObservation, false),
        "weak observation was treated as an owning escape");
}

void test_global_storage_escapes()
{
    const auto ir = analyze(
        "class Item {}\n"
        "global_item := Item()\n");
    const auto& value = allocation(ir, "<top-level>");
    expect(value.state == toro::IREscapeState::Escapes
            && has_reason(value, toro::IREscapeReason::GlobalStorage),
        "top-level managed storage was not classified as escaping");
}

void test_collection_storage_escapes()
{
    const auto ir = analyze(
        "class Item {}\n"
        "function list_value() {\n"
        "    values := List<Item>()\n"
        "    value := Item()\n"
        "    values.add(value)\n"
        "}\n"
        "function map_value() {\n"
        "    values := Map<string, Item>()\n"
        "    value := Item()\n"
        "    values[\"item\"] = value\n"
        "}\n"
        "function array_value() {\n"
        "    values := Array<Item>(Item())\n"
        "}\n");
    for (const auto suffix : {"list_value()", "map_value()", "array_value()"}) {
        const auto& value = allocation(ir, suffix);
        expect(value.state == toro::IREscapeState::Escapes
                && has_reason(
                    value, toro::IREscapeReason::CollectionStorage),
            "collection-stored allocation did not escape");
    }
}

void test_control_flow_merges_conservatively()
{
    const auto ir = analyze(
        "class Item { public name: string }\n"
        "class World { public item: Item? }\n"
        "function branch_escape(world: World, flag: bool) {\n"
        "    value := Item(name: \"branch\")\n"
        "    if flag {\n"
        "        alias := value\n"
        "        world.item = alias\n"
        "    }\n"
        "}\n"
        "function branch_local(flag: bool) {\n"
        "    value := Item(name: \"local\")\n"
        "    if flag { print(value.name) }\n"
        "}\n"
        "function loop_local(flag: bool) {\n"
        "    value := Item(name: \"loop\")\n"
        "    while flag {\n"
        "        print(value.name)\n"
        "        stop\n"
        "    }\n"
        "}\n"
        "function loop_escape(world: World, flag: bool) {\n"
        "    value := Item(name: \"eventual\")\n"
        "    current: Item? = null\n"
        "    while flag {\n"
        "        world.item = current\n"
        "        current = value\n"
        "    }\n"
        "}\n");
    expect(allocation(ir, "branch_escape(World,bool)").state
            == toro::IREscapeState::Escapes,
        "escaping branch path was lost during merge");
    expect(allocation(ir, "branch_local(bool)").state
            == toro::IREscapeState::DoesNotEscape,
        "branch-only observation caused escape");
    expect(allocation(ir, "loop_local(bool)").state
            == toro::IREscapeState::DoesNotEscape,
        "known loop observation caused escape");
    expect(allocation(ir, "loop_escape(World,bool)").state
            == toro::IREscapeState::Escapes,
        "loop-carried alias escape was missed");
}

void test_nullable_upcast_interface_and_generic_aliases()
{
    const auto ir = analyze(
        "interface Named { function name() -> string }\n"
        "class Base {\n"
        "    public virtual function name() -> string { return \"base\" }\n"
        "}\n"
        "class Derived : Base implements Named {\n"
        "    public override function name() -> string { return \"derived\" }\n"
        "}\n"
        "class Box<T> { public value: T }\n"
        "function nullable_alias() {\n"
        "    value: Derived? = Derived()\n"
        "    copy := value\n"
        "}\n"
        "function upcast_alias() {\n"
        "    value := Derived()\n"
        "    base: Base = value\n"
        "    print(base.name())\n"
        "}\n"
        "function interface_alias() {\n"
        "    value := Derived()\n"
        "    named: Named = value\n"
        "}\n"
        "function generic_alias() {\n"
        "    value := Box<int>(value: 1)\n"
        "    copy := value\n"
        "    print(copy.value)\n"
        "}\n");
    expect(allocation(ir, "nullable_alias()").state
            == toro::IREscapeState::DoesNotEscape,
        "nullable local alias escaped");
    expect(allocation(ir, "upcast_alias()").state
            == toro::IREscapeState::Unknown,
        "virtual dispatch was not conservative");
    const auto& interface_value = allocation(ir, "interface_alias()");
    expect(interface_value.state == toro::IREscapeState::DoesNotEscape
            && has_reason(
                interface_value, toro::IREscapeReason::InterfaceAlias, false),
        "local interface wrapper was treated as an escape");
    expect(allocation(ir, "generic_alias()").state
            == toro::IREscapeState::DoesNotEscape,
        "concrete generic class alias escaped");
}

void test_direct_dynamic_and_foreign_calls()
{
    const auto ir = analyze(
        "extern \"C\" function puts(text: c_string) -> c_int\n"
        "interface Readable { function read() -> int }\n"
        "class Item implements Readable {\n"
        "    public value: int\n"
        "    public function read() -> int { return self.value }\n"
        "}\n"
        "class Sink { public item: Item? }\n"
        "function observe(value: Item) { print(value.value) }\n"
        "function retain(value: Item, sink: Sink) { sink.item = value }\n"
        "function recurse(value: Item, count: int) {\n"
        "    if count > 0 { recurse(value, count - 1) }\n"
        "    else { print(value.value) }\n"
        "}\n"
        "function direct_observe() {\n"
        "    value := Item(value: 1)\n"
        "    observe(value)\n"
        "}\n"
        "function maybe_escape(sink: Sink) {\n"
        "    value := Item(value: 2)\n"
        "    retain(value, sink)\n"
        "}\n"
        "function recursive_observe() {\n"
        "    value := Item(value: 5)\n"
        "    recurse(value, 2)\n"
        "}\n"
        "function interface_call() {\n"
        "    value := Item(value: 3)\n"
        "    readable: Readable = value\n"
        "    print(readable.read())\n"
        "}\n"
        "function foreign_barrier() {\n"
        "    value := Item(value: 4)\n"
        "    puts(\"foreign\" as c_string)\n"
        "    print(value.value)\n"
        "}\n");
    expect(allocation(ir, "direct_observe()").state
            == toro::IREscapeState::DoesNotEscape,
        "known observing call was treated as escaping");
    expect(allocation(ir, "maybe_escape(Sink)").state
            == toro::IREscapeState::Unknown,
        "possibly escaping direct call was not conservative");
    expect(allocation(ir, "interface_call()").state
            == toro::IREscapeState::Unknown,
        "interface call was not conservative");
    expect(allocation(ir, "foreign_barrier()").state
            == toro::IREscapeState::Unknown,
        "foreign call did not conservatively classify live references");
    expect(allocation(ir, "recursive_observe()").state
            == toro::IREscapeState::DoesNotEscape,
        "deterministic recursive observation was treated as escaping");
}

void test_result_enum_payload_aliases_and_determinism()
{
    constexpr std::string_view source =
        "class Item {}\n"
        "enum Payload { item(Item) }\n"
        "function result_value() -> Result<Item, string> {\n"
        "    value := Item()\n"
        "    return ok(value)\n"
        "}\n"
        "function enum_value() -> Payload {\n"
        "    value := Item()\n"
        "    return Payload::item(value)\n"
        "}\n";
    const auto first = analyze(source);
    const auto second = analyze(source);
    const auto& result = allocation(first, "result_value()");
    expect(result.state == toro::IREscapeState::Escapes
            && has_reason(result, toro::IREscapeReason::ResultPayload, false)
            && has_reason(result, toro::IREscapeReason::Return),
        "Result payload alias did not escape through return");
    const auto& enumeration = allocation(first, "enum_value()");
    expect(enumeration.state == toro::IREscapeState::Escapes
            && has_reason(enumeration, toro::IREscapeReason::EnumPayload, false)
            && has_reason(enumeration, toro::IREscapeReason::Return),
        "enum payload alias did not escape through return");
    expect(toro::dump_ir(first, false, false, true)
            == toro::dump_ir(second, false, false, true),
        "escape inspection was not deterministic");
}

void test_analysis_does_not_change_arc_or_generated_c()
{
    constexpr std::string_view source =
        "class Item {\n"
        "    public value: int\n"
        "    destroy() { print(\"destroyed\") }\n"
        "}\n"
        "function make() -> Item {\n"
        "    value := Item(value: 7)\n"
        "    return value\n"
        "}\n"
        "function main() {\n"
        "    item := make()\n"
        "    print(item.value)\n"
        "}\n";
    auto baseline = analyze(source);
    baseline.escape_allocations.clear();
    baseline.escape_metrics = {};
    auto annotated = analyze(source);
    expect(toro::dump_ir(baseline, true) == toro::dump_ir(annotated, true),
        "escape analysis changed effect inspection output");
    toro::ArcMaterializer().materialize(baseline);
    toro::OwnershipTransferAnalyzer().analyze(baseline);
    toro::ArcOptimizer().optimize(baseline);
    toro::ArcMaterializer().materialize(annotated);
    toro::OwnershipTransferAnalyzer().analyze(annotated);
    toro::ArcOptimizer().optimize(annotated);
    expect(baseline.arc_metrics.retains_after
                == annotated.arc_metrics.retains_after
            && baseline.arc_metrics.releases_after
                == annotated.arc_metrics.releases_after
            && baseline.arc_metrics.ownership_transfers
                == annotated.arc_metrics.ownership_transfers,
        "escape analysis changed ARC metrics");
    expect(toro::CGenerator().generate(baseline)
            == toro::CGenerator().generate(annotated),
        "escape annotations changed generated C");
}

} // namespace

int main()
{
    try {
        test_local_aliases_do_not_escape();
        test_return_strong_field_and_weak_observation();
        test_global_storage_escapes();
        test_collection_storage_escapes();
        test_control_flow_merges_conservatively();
        test_nullable_upcast_interface_and_generic_aliases();
        test_direct_dynamic_and_foreign_calls();
        test_result_enum_payload_aliases_and_determinism();
        test_analysis_does_not_change_arc_or_generated_c();
    } catch (const std::exception& error) {
        std::cerr << "escape analyzer test failure: " << error.what() << '\n';
        return 1;
    }
    std::cout << "escape analyzer tests passed\n";
    return 0;
}
