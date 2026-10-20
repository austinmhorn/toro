#include "toro/ArcOptimizer.hpp"
#include "toro/CGenerator.hpp"
#include "toro/EffectAnalyzer.hpp"
#include "toro/IRLowerer.hpp"
#include "toro/Lexer.hpp"
#include "toro/NativeCompiler.hpp"
#include "toro/Parser.hpp"
#include "toro/SemanticAnalyzer.hpp"
#include "toro/TypeChecker.hpp"

#include <cstdio>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace {

void expect(bool condition, std::string_view message)
{
    if (!condition) throw std::runtime_error(std::string(message));
}

toro::IRProgram lower(std::string_view source, bool optimize)
{
    auto tokens = toro::Lexer(source).tokenize();
    auto program = toro::Parser(std::move(tokens)).parse_program();
    toro::SemanticAnalyzer().analyze(program);
    toro::TypeChecker().check(program);
    auto ir = toro::IRLowerer().lower(program);
    toro::EffectAnalyzer().analyze(ir);
    toro::ArcMaterializer().materialize(ir);
    if (optimize) {
        toro::OwnershipTransferAnalyzer().analyze(ir);
        toro::ArcOptimizer().optimize(ir);
    }
    return ir;
}

const toro::IRCallableArcSchedule& schedule(
    const toro::IRProgram& program,
    std::string_view suffix)
{
    for (const auto& candidate : program.arc_schedules) {
        if (candidate.callable_identity.ends_with(suffix)) return candidate;
    }
    throw std::runtime_error("missing ARC schedule");
}

bool has_borrow(const toro::IRCallableArcSchedule& value)
{
    for (const auto& operation : value.optimized_operations) {
        if (operation.kind == toro::IRArcOperationKind::BorrowedTemporary
            && operation.reason
                == "locally proven non-escaping parameter borrow") {
            return true;
        }
    }
    return false;
}

bool has_transfer(
    const toro::IRCallableArcSchedule& value,
    std::string_view description = {})
{
    for (const auto& operation : value.optimized_operations) {
        if (operation.kind != toro::IRArcOperationKind::OwnershipTransfer) {
            continue;
        }
        if (description.empty() || operation.value == description) return true;
    }
    return false;
}

std::pair<int, std::string> capture_run(std::string_view c_source)
{
#if defined(_WIN32)
    static_cast<void>(c_source);
    throw std::runtime_error("ARC differential test requires a POSIX host");
#else
    int output_pipe[2];
    if (::pipe(output_pipe) != 0) throw std::runtime_error("pipe failed");
    std::fflush(stdout);
    const int saved_stdout = ::dup(STDOUT_FILENO);
    if (saved_stdout == -1 || ::dup2(output_pipe[1], STDOUT_FILENO) == -1) {
        throw std::runtime_error("stdout redirection failed");
    }
    ::close(output_pipe[1]);
    int status = 0;
    try {
        status = toro::NativeCompiler().run(c_source);
    } catch (...) {
        std::fflush(stdout);
        static_cast<void>(::dup2(saved_stdout, STDOUT_FILENO));
        ::close(saved_stdout);
        ::close(output_pipe[0]);
        throw;
    }
    std::fflush(stdout);
    static_cast<void>(::dup2(saved_stdout, STDOUT_FILENO));
    ::close(saved_stdout);
    std::string output;
    char buffer[256];
    ssize_t count = 0;
    while ((count = ::read(output_pipe[0], buffer, sizeof(buffer))) > 0) {
        output.append(buffer, static_cast<std::size_t>(count));
    }
    ::close(output_pipe[0]);
    return {status, output};
#endif
}

void test_materialization_and_local_parameter_elision()
{
    constexpr std::string_view source =
        "class Box {\n"
        "    public value: int\n"
        "}\n"
        "function read(box: Box) -> int {\n"
        "    return box.value\n"
        "}\n";
    const auto baseline = lower(source, false);
    const auto optimized = lower(source, true);
    expect(baseline.arc_stage == toro::IRArcStage::Materialized,
        "baseline ARC was not materialized");
    expect(optimized.arc_stage == toro::IRArcStage::Optimized,
        "optimized ARC stage was not recorded");
    expect(baseline.arc_metrics.retains_before
            == baseline.arc_metrics.retains_after,
        "unoptimized ARC changed retain counts");
    expect(optimized.arc_metrics.retains_after
            < optimized.arc_metrics.retains_before,
        "obvious parameter retain was not elided");
    expect(optimized.arc_metrics.releases_after
            < optimized.arc_metrics.releases_before,
        "matching parameter release was not elided");
    expect(optimized.arc_metrics.elided_pairs == 1,
        "unexpected number of elided pairs");
    expect(has_borrow(schedule(optimized, "read(Box)")),
        "optimized parameter was not represented as borrowed");
    const auto dump = toro::dump_ir(optimized, false, true);
    expect(dump.find("retain box [elided]") != std::string::npos,
        "ARC dump did not explain the elided retain");
    expect(dump.find("elided-pairs 1") != std::string::npos,
        "ARC dump did not expose metrics");
}

void test_escape_dynamic_generic_and_storage_barriers()
{
    const auto ir = lower(
        "interface Readable {\n"
        "    function read() -> int\n"
        "}\n"
        "class Box implements Readable {\n"
        "    public value: int\n"
        "    public function read() -> int {\n"
        "        return self.value\n"
        "    }\n"
        "}\n"
        "function escape(box: Box) -> Box {\n"
        "    return box\n"
        "}\n"
        "function dynamic(value: Readable) -> int {\n"
        "    return value.read()\n"
        "}\n"
        "function identity<T>(value: T) -> T {\n"
        "    return value\n"
        "}\n"
        "function store(box: Box) {\n"
        "    copy := box\n"
        "    print(copy.value)\n"
        "}\n",
        true);
    expect(!has_borrow(schedule(ir, "escape(Box)")),
        "escaping return was treated as borrowed");
    expect(!has_borrow(schedule(ir, "dynamic(Readable)")),
        "dynamic dispatch was treated as borrowed");
    expect(!has_borrow(schedule(ir, "identity(T)")),
        "generic parameter was optimized without concrete proof");
    expect(!has_borrow(schedule(ir, "store(Box)")),
        "stored parameter was treated as borrowed");
}

void test_nullable_observation_and_foreign_barrier()
{
    const auto nullable = lower(
        "class Box {\n"
        "}\n"
        "function missing(box: Box?) -> bool {\n"
        "    return box == null\n"
        "}\n",
        true);
    expect(has_borrow(schedule(nullable, "missing(Box?)")),
        "nullable identity observation did not elide ARC pair");

    const auto foreign = lower(
        "extern \"C\" function puts(text: c_string) -> c_int\n"
        "class Box {\n"
        "    public value: int\n"
        "}\n"
        "function observe(box: Box) -> int {\n"
        "    puts(\"observe\" as c_string)\n"
        "    return box.value\n"
        "}\n",
        true);
    expect(!has_borrow(schedule(foreign, "observe(Box)")),
        "foreign-call effect barrier was ignored");
}

void test_weak_fields_and_managed_payloads_are_explicit()
{
    const auto ir = lower(
        "class Parent {\n"
        "}\n"
        "class Child {\n"
        "    weak parent: Parent?\n"
        "}\n"
        "enum Item {\n"
        "    box(Parent)\n"
        "    none\n"
        "}\n"
        "function main() {\n"
        "    parent := Parent()\n"
        "    item := Item::box(parent)\n"
        "    values := List<Parent>(parent)\n"
        "    print(values.count)\n"
        "}\n",
        true);
    const auto dump = toro::dump_ir(ir, false, true);
    expect(dump.find("weak slot observes control block without retaining target")
            != std::string::npos,
        "weak field ownership was not explicit");
    expect(dump.find("managed payload ownership") != std::string::npos,
        "enum class payload ownership was not explicit");
    expect(dump.find("reference stored in collection") != std::string::npos,
        "collection element ownership was not explicit");
}

void test_backend_consumes_arc_schedule_and_differential_lifecycle()
{
    constexpr std::string_view source =
        "class Box {\n"
        "    public value: int\n"
        "    destroy() { print(\"destroyed\") }\n"
        "}\n"
        "function read(box: Box) -> int { return box.value }\n"
        "function main() {\n"
        "    box := Box(value: 7)\n"
        "    print(read(box))\n"
        "}\n";
    const auto baseline = lower(source, false);
    const auto optimized = lower(source, true);
    const auto baseline_c = toro::CGenerator().generate(baseline);
    const auto optimized_c = toro::CGenerator().generate(optimized);
    expect(baseline_c != optimized_c,
        "backend ignored the optimized ARC schedule");
    const auto [baseline_status, baseline_output] = capture_run(baseline_c);
    const auto [optimized_status, optimized_output] = capture_run(optimized_c);
    expect(baseline_status == 0 && optimized_status == 0,
        "differential ARC program failed");
    expect(baseline_output == optimized_output,
        "optimized ARC changed observable lifetime behavior");
    expect(optimized_output == "7\ndestroyed\n",
        "destruction trace was not exactly once and ordered");
    const auto second = lower(source, true);
    expect(toro::dump_ir(optimized, false, true)
            == toro::dump_ir(second, false, true),
        "ARC inspection output was not deterministic");
}

void test_final_use_transfers_for_locals_and_returns()
{
    const auto ir = lower(
        "class Box {\n"
        "    public value: int\n"
        "}\n"
        "function local_copy() {\n"
        "    first := Box(value: 1)\n"
        "    second := first\n"
        "    print(second.value)\n"
        "    third := Box(value: 4)\n"
        "    fourth := Box(value: 5)\n"
        "    fourth = third\n"
        "    print(fourth.value)\n"
        "}\n"
        "function return_local() -> Box {\n"
        "    value := Box(value: 2)\n"
        "    return value\n"
        "}\n"
        "function return_temporary() -> Box {\n"
        "    return Box(value: 3)\n"
        "}\n",
        true);
    expect(has_transfer(schedule(ir, "local_copy()"), "first -> second"),
        "final local copy did not transfer ownership");
    expect(has_transfer(schedule(ir, "local_copy()"), "third -> fourth"),
        "final local reassignment did not transfer ownership");
    expect(has_transfer(schedule(ir, "return_local()"), "value -> return"),
        "local return did not transfer ownership");
    expect(has_transfer(
            schedule(ir, "return_temporary()"), "<temporary> -> return"),
        "temporary return was not represented as a transfer");
    expect(ir.arc_metrics.ownership_transfers == 4,
        "ownership-transfer metric did not count transfers");
    expect(ir.arc_metrics.retains_after < ir.arc_metrics.retains_before
            && ir.arc_metrics.releases_after < ir.arc_metrics.releases_before,
        "ownership transfers did not reduce retain/release traffic");
    const auto dump = toro::dump_ir(ir, false, true);
    expect(dump.find("transfer first -> second") != std::string::npos,
        "ARC dump did not expose local transfer");
    expect(dump.find("ownership-transfers 4") != std::string::npos,
        "ARC dump did not expose transfer metrics");
}

void test_managed_storage_transfers_and_weak_exclusion()
{
    const auto ir = lower(
        "class Box {\n"
        "}\n"
        "class Holder {\n"
        "    public item: Box?\n"
        "    public weak observed: Box?\n"
        "}\n"
        "function field_store(holder: Holder) {\n"
        "    value := Box()\n"
        "    holder.item = value\n"
        "}\n"
        "function weak_store(holder: Holder) {\n"
        "    value := Box()\n"
        "    holder.observed = value\n"
        "}\n"
        "function list_store() {\n"
        "    values := List<Box>()\n"
        "    value := Box()\n"
        "    values.add(value)\n"
        "}\n"
        "function map_store() {\n"
        "    values := Map<string, Box>()\n"
        "    value := Box()\n"
        "    values[\"box\"] = value\n"
        "}\n"
        "function array_store() {\n"
        "    values := Array<Box>(Box())\n"
        "    value := Box()\n"
        "    values[0] = value\n"
        "}\n",
        true);
    expect(has_transfer(schedule(ir, "field_store(Holder)"),
               "value -> field item"),
        "strong-field final use did not transfer ownership");
    expect(!has_transfer(schedule(ir, "weak_store(Holder)")),
        "weak-field assignment incorrectly transferred strong ownership");
    expect(has_transfer(schedule(ir, "list_store()"),
               "value -> collection element"),
        "List insertion did not transfer final-use ownership");
    expect(has_transfer(schedule(ir, "map_store()"),
               "value -> collection element"),
        "Map insertion did not transfer final-use ownership");
    expect(has_transfer(schedule(ir, "array_store()"),
               "value -> collection element"),
        "Array replacement did not transfer final-use ownership");
}

void test_liveness_and_control_flow_are_conservative()
{
    const auto ir = lower(
        "class Box {\n"
        "    public value: int\n"
        "}\n"
        "function used_after() {\n"
        "    first := Box(value: 1)\n"
        "    second := first\n"
        "    print(first.value)\n"
        "    print(second.value)\n"
        "}\n"
        "function branch(flag: bool) {\n"
        "    first := Box(value: 1)\n"
        "    second := first\n"
        "    if flag {\n"
        "        print(first.value)\n"
        "    }\n"
        "    print(second.value)\n"
        "}\n"
        "function loop(flag: bool) {\n"
        "    first := Box(value: 1)\n"
        "    second := first\n"
        "    while flag {\n"
        "        print(first.value)\n"
        "        stop\n"
        "    }\n"
        "    print(second.value)\n"
        "}\n",
        true);
    expect(!has_transfer(schedule(ir, "used_after()")),
        "source use after copy was ignored");
    expect(!has_transfer(schedule(ir, "branch(bool)")),
        "branch-sensitive liveness was optimized without proof");
    expect(!has_transfer(schedule(ir, "loop(bool)")),
        "loop liveness was optimized without proof");
}

void test_nullable_upcast_interface_and_generic_transfers()
{
    const auto ir = lower(
        "interface Named { function name() -> string }\n"
        "class Base {\n"
        "    public virtual function name() -> string { return \"base\" }\n"
        "}\n"
        "class Derived : Base implements Named {\n"
        "    public override function name() -> string { return \"derived\" }\n"
        "}\n"
        "class Box<T> { public value: T }\n"
        "function nullable(value: Derived?) { copy := value }\n"
        "function upcast() {\n"
        "    value := Derived()\n"
        "    base: Base = value\n"
        "    print(base.name())\n"
        "}\n"
        "function interface_value() {\n"
        "    value := Derived()\n"
        "    named: Named = value\n"
        "    print(named.name())\n"
        "}\n"
        "function generic_value() {\n"
        "    value := Box<int>(value: 1)\n"
        "    copy := value\n"
        "    print(copy.value)\n"
        "}\n",
        true);
    expect(has_transfer(schedule(ir, "nullable(Derived?)"), "value -> copy"),
        "nullable class reference did not transfer");
    expect(has_transfer(schedule(ir, "upcast()"), "value -> base"),
        "derived-to-base upcast did not transfer");
    expect(has_transfer(
            schedule(ir, "interface_value()"), "value -> named"),
        "class-to-interface conversion did not transfer");
    expect(has_transfer(
            schedule(ir, "generic_value()"), "value -> copy"),
        "concrete generic class did not transfer");
}

void test_calls_remain_conservative_without_consuming_contracts()
{
    const auto ir = lower(
        "extern \"C\" function puts(text: c_string) -> c_int\n"
        "interface Readable { function read() -> int }\n"
        "class Box implements Readable {\n"
        "    public value: int\n"
        "    public function read() -> int { return self.value }\n"
        "}\n"
        "function observe(value: Box) { print(value.value) }\n"
        "function calls() {\n"
        "    observe(Box(value: 1))\n"
        "    value := Box(value: 2)\n"
        "    print(value.read())\n"
        "    puts(\"barrier\" as c_string)\n"
        "}\n",
        true);
    expect(!has_transfer(schedule(ir, "calls()")),
        "call argument or dispatch was transferred without a consuming contract");
}

void test_transfer_backend_differential_storage_and_destruction()
{
    constexpr std::string_view source =
        "class Base {\n"
        "    public value: int\n"
        "    destroy() { print(\"base destroyed\") }\n"
        "}\n"
        "class Derived : Base {\n"
        "    destroy() { print(\"derived destroyed\") }\n"
        "}\n"
        "class Holder { public item: Base = Base() }\n"
        "function make() -> Base {\n"
        "    value := Derived(value: 7)\n"
        "    return value\n"
        "}\n"
        "function main() {\n"
        "    holder := Holder()\n"
        "    value := make()\n"
        "    holder.item = value\n"
        "    print(holder.item.value)\n"
        "}\n";
    const auto baseline = lower(source, false);
    const auto optimized = lower(source, true);
    const auto [baseline_status, baseline_output] =
        capture_run(toro::CGenerator().generate(baseline));
    const auto [optimized_status, optimized_output] =
        capture_run(toro::CGenerator().generate(optimized));
    expect(baseline_status == 0 && optimized_status == 0,
        "ownership-transfer differential program failed");
    expect(baseline_output == optimized_output,
        "ownership transfer changed observable behavior");
    expect(optimized_output
            == "base destroyed\n7\nderived destroyed\nbase destroyed\n",
        "ownership transfer changed destruction count or order: "
            + optimized_output);
}

void test_transfer_backend_differential_collections_and_weak_invalidation()
{
    constexpr std::string_view source =
        "class Item {\n"
        "    public value: int\n"
        "    destroy() { print(self.value) }\n"
        "}\n"
        "class Owner { public item: Item? }\n"
        "class Watcher { public weak item: Item? }\n"
        "function main() {\n"
        "    values := List<Item>()\n"
        "    listed := Item(value: 1)\n"
        "    values.add(listed)\n"
        "    mapped := Map<string, Item>()\n"
        "    entry := Item(value: 2)\n"
        "    mapped[\"item\"] = entry\n"
        "    watcher := Watcher()\n"
        "    {\n"
        "        owner := Owner()\n"
        "        observed := Item(value: 3)\n"
        "        watcher.item = observed\n"
        "        owner.item = observed\n"
        "        print(watcher.item != null)\n"
        "    }\n"
        "    print(watcher.item == null)\n"
        "    array := Array<Item>(Item(value: 4))\n"
        "    replacement := Item(value: 5)\n"
        "    array[0] = replacement\n"
        "}\n";
    const auto baseline = lower(source, false);
    const auto optimized = lower(source, true);
    const auto [baseline_status, baseline_output] =
        capture_run(toro::CGenerator().generate(baseline));
    const auto [optimized_status, optimized_output] =
        capture_run(toro::CGenerator().generate(optimized));
    expect(baseline_status == 0 && optimized_status == 0,
        "collection/weak differential program failed");
    expect(baseline_output == optimized_output,
        "transfer changed collection ownership or weak invalidation");
    expect(optimized_output == "true\n3\ntrue\n4\n5\n2\n1\n",
        "collection/weak destruction trace changed: " + optimized_output);
}

void test_strong_cycle_behavior_is_unchanged()
{
    constexpr std::string_view source =
        "class Node {\n"
        "    public next: Node?\n"
        "    destroy() { print(\"destroyed\") }\n"
        "}\n"
        "function main() {\n"
        "    first := Node()\n"
        "    second := Node()\n"
        "    first.next = second\n"
        "    second.next = first\n"
        "}\n";
    const auto baseline = lower(source, false);
    const auto optimized = lower(source, true);
    const auto [baseline_status, baseline_output] =
        capture_run(toro::CGenerator().generate(baseline));
    const auto [optimized_status, optimized_output] =
        capture_run(toro::CGenerator().generate(optimized));
    expect(baseline_status == 0 && optimized_status == 0,
        "strong-cycle differential program failed");
    expect(baseline_output == optimized_output && optimized_output.empty(),
        "ownership transfer changed the documented strong-cycle limitation");
}

} // namespace

int main()
{
    try {
        test_materialization_and_local_parameter_elision();
        test_escape_dynamic_generic_and_storage_barriers();
        test_nullable_observation_and_foreign_barrier();
        test_weak_fields_and_managed_payloads_are_explicit();
        test_backend_consumes_arc_schedule_and_differential_lifecycle();
        test_final_use_transfers_for_locals_and_returns();
        test_managed_storage_transfers_and_weak_exclusion();
        test_liveness_and_control_flow_are_conservative();
        test_nullable_upcast_interface_and_generic_transfers();
        test_calls_remain_conservative_without_consuming_contracts();
        test_transfer_backend_differential_storage_and_destruction();
        test_transfer_backend_differential_collections_and_weak_invalidation();
        test_strong_cycle_behavior_is_unchanged();
    } catch (const std::exception& error) {
        std::cerr << "ARC optimizer test failure: " << error.what() << '\n';
        return 1;
    }
    std::cout << "ARC optimizer tests passed\n";
    return 0;
}
