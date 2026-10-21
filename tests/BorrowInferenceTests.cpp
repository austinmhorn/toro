#include "toro/ArcOptimizer.hpp"
#include "toro/BorrowInferenceAnalyzer.hpp"
#include "toro/CGenerator.hpp"
#include "toro/EffectAnalyzer.hpp"
#include "toro/EscapeAnalyzer.hpp"
#include "toro/IRLowerer.hpp"
#include "toro/Lexer.hpp"
#include "toro/NativeCompiler.hpp"
#include "toro/OwnershipSummaryAnalyzer.hpp"
#include "toro/Parser.hpp"
#include "toro/SemanticAnalyzer.hpp"
#include "toro/StorageDecisionAnalyzer.hpp"
#include "toro/TypeChecker.hpp"

#include <algorithm>
#include <cstdio>
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

toro::IRProgram lower(std::string_view source)
{
    auto program = toro::Parser(toro::Lexer(source).tokenize()).parse_program();
    toro::SemanticAnalyzer().analyze(program);
    toro::TypeChecker().check(program);
    return toro::IRLowerer().lower(program);
}

toro::IRProgram analyze_ir(
    toro::IRProgram ir,
    bool enable_borrowing = true)
{
    toro::EffectAnalyzer().analyze(ir);
    toro::OwnershipSummaryAnalyzer().analyze(ir);
    toro::EscapeAnalyzer().analyze(ir);
    toro::StorageDecisionAnalyzer().analyze(ir);
    toro::ArcMaterializer().materialize(ir);
    toro::OwnershipTransferAnalyzer().analyze(ir);
    toro::BorrowInferenceAnalyzer().analyze(ir, enable_borrowing);
    toro::ArcOptimizer().optimize(ir);
    return ir;
}

toro::IRProgram analyze(std::string_view source, bool enable_borrowing = true)
{
    return analyze_ir(lower(source), enable_borrowing);
}

const toro::IRParameterOwnershipSummary& summary(
    const toro::IRProgram& program,
    std::string_view callable_suffix,
    std::string_view parameter)
{
    for (const auto& callable : program.ownership_summaries) {
        if (!callable.callable_identity.ends_with(callable_suffix)) continue;
        for (const auto& candidate : callable.parameters) {
            if (candidate.parameter_name == parameter) return candidate;
        }
    }
    throw std::runtime_error("missing borrow summary");
}

const toro::IRCallableArcSchedule& schedule(
    const toro::IRProgram& program,
    std::string_view callable_suffix)
{
    for (const auto& candidate : program.arc_schedules) {
        if (candidate.callable_identity.ends_with(callable_suffix)) {
            return candidate;
        }
    }
    throw std::runtime_error("missing ARC schedule");
}

bool has_borrow(
    const toro::IRCallableArcSchedule& value,
    std::string_view fragment)
{
    for (const auto& operation : value.optimized_operations) {
        if (operation.kind == toro::IRArcOperationKind::BorrowedTemporary
            && operation.value.find(fragment) != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::pair<int, std::string> capture_run(std::string_view c_source)
{
#if defined(_WIN32)
    static_cast<void>(c_source);
    throw std::runtime_error("borrow differential test requires a POSIX host");
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

void test_parameter_specific_direct_and_forwarded_borrows()
{
    const auto ir = analyze(
        "extern \"C\" function puts(text: c_string) -> c_int\n"
        "class Box { public value: int }\n"
        "class Sink { public value: Box? }\n"
        "function observe(value: Box) -> int {\n"
        "    puts(\"unrelated effect\" as c_string)\n"
        "    return value.value\n"
        "}\n"
        "function retain(value: Box, sink: Sink) { sink.value = value }\n"
        "function collect(value: Box, values: List<Box>) { values.add(value) }\n"
        "function forward(value: Box, sink: Sink) { retain(value, sink) }\n"
        "function caller() {\n"
        "    value := Box(value: 7)\n"
        "    print(observe(value))\n"
        "    print(value.value)\n"
        "}\n");
    expect(toro::ownership_is_observed_only(
            summary(ir, "observe(Box)", "value")),
        "unrelated foreign effect blocked a parameter-specific borrow");
    expect(!toro::ownership_is_observed_only(
            summary(ir, "retain(Box,Sink)", "value")),
        "strong-field storage was treated as observation-only");
    expect(!toro::ownership_is_observed_only(
            summary(ir, "forward(Box,Sink)", "value")),
        "forwarding to an escaping parameter was borrowed");
    expect(!toro::ownership_is_observed_only(
            summary(ir, "collect(Box,List<Box>)", "value")),
        "collection insertion was treated as observation-only");
    expect(has_borrow(schedule(ir, "caller()"), "value -> observe.value"),
        "direct observing call did not record its argument borrow");
}

void test_methods_virtual_interfaces_and_generics()
{
    const auto ir = analyze(
        "interface Named { function name() -> string }\n"
        "class Base {\n"
        "    public virtual function name() -> string { return \"base\" }\n"
        "    public function inspect(other: Base) -> string { return other.name() }\n"
        "    public function generic<T>(other: T) { }\n"
        "}\n"
        "class Derived : Base implements Named {\n"
        "    public override function name() -> string { return \"derived\" }\n"
        "}\n"
        "function observe<T>(value: T) { }\n"
        "function calls() {\n"
        "    value := Derived()\n"
        "    base: Base = value\n"
        "    named: Named = value\n"
        "    print(base.name())\n"
        "    print(named.name())\n"
        "    print(base.inspect(value))\n"
        "    base.generic<Derived>(value)\n"
        "    observe<Derived>(value)\n"
        "}\n");
    const auto& calls = schedule(ir, "calls()");
    expect(has_borrow(calls, "value -> base"),
        "derived-to-base local view was not borrowed");
    expect(has_borrow(calls, "value -> named"),
        "class-backed interface local view was not borrowed");
    expect(has_borrow(calls, "base -> name.self"),
        "safe virtual receiver borrow was not recorded");
    expect(has_borrow(calls, "named -> name.self"),
        "safe interface receiver borrow was not recorded");
    expect(has_borrow(calls, "value -> inspect.other"),
        "method parameter borrow was not recorded");
    expect(has_borrow(calls, "value -> generic.other"),
        "generic method borrow was not recorded");
    expect(has_borrow(calls, "value -> observe.value"),
        "generic function borrow was not recorded");
}

void test_unsafe_dynamic_and_weak_cases_remain_owning()
{
    const auto ir = analyze(
        "class Sink { public value: Base? }\n"
        "class Base {\n"
        "    public virtual function visit(sink: Sink) { }\n"
        "}\n"
        "class Derived : Base {\n"
        "    public override function visit(sink: Sink) { sink.value = self }\n"
        "}\n"
        "class Watcher { public weak value: Base? }\n"
        "function inspect(value: Base?) { print(value == null) }\n"
        "function dynamic(value: Base, sink: Sink) { value.visit(sink) }\n"
        "function weak_call(watcher: Watcher) { inspect(watcher.value) }\n");
    expect(!has_borrow(schedule(ir, "dynamic(Base,Sink)"), "visit.self"),
        "unsafe virtual target was treated as a receiver borrow");
    expect(!has_borrow(schedule(ir, "weak_call(Watcher)"),
            "member value -> inspect.value"),
        "weak-reference access was modeled as an ordinary borrow");
}

void test_foreign_and_unknown_call_boundaries_remain_owning()
{
    constexpr std::string_view source =
        "class Box { public value: int }\n"
        "function sink(value: Box) { }\n"
        "function forward(value: Box) { sink(value) }\n";
    auto foreign = lower(source);
    auto& foreign_call = foreign.declarations.back().body->children.front();
    foreign_call.kind = toro::IROperationKind::ForeignCall;
    foreign = analyze_ir(std::move(foreign));
    expect(std::ranges::find(summary(foreign, "forward(Box)", "value").behaviors,
            toro::IROwnershipBehavior::Unknown)
            != summary(foreign, "forward(Box)", "value").behaviors.end(),
        "foreign boundary was treated as borrow-safe");

    auto unknown = lower(source);
    auto& unknown_call = unknown.declarations.back().body->children.front();
    unknown_call.selected_declaration.clear();
    unknown = analyze_ir(std::move(unknown));
    expect(std::ranges::find(summary(unknown, "forward(Box)", "value").behaviors,
            toro::IROwnershipBehavior::Unknown)
            != summary(unknown, "forward(Box)", "value").behaviors.end(),
        "unresolved call boundary was treated as borrow-safe");
}

void test_local_aliases_and_transfer_distinction()
{
    const auto ir = analyze(
        "class Box { public value: int }\n"
        "function make() -> Box { return Box(value: 1) }\n"
        "function aliases(flag: bool) {\n"
        "    owner := make()\n"
        "    observed := owner\n"
        "    print(observed.value)\n"
        "    print(owner.value)\n"
        "    uncertain := owner\n"
        "    if flag { print(uncertain.value) }\n"
        "    replaced_owner := make()\n"
        "    stale_candidate := replaced_owner\n"
        "    replaced_owner = make()\n"
        "    print(stale_candidate.value)\n"
        "    loop_owner := make()\n"
        "    loop_candidate := loop_owner\n"
        "    while flag {\n"
        "        print(loop_candidate.value)\n"
        "        stop\n"
        "    }\n"
        "    print(loop_owner.value)\n"
        "    moved := make()\n"
        "    destination := moved\n"
        "    print(destination.value)\n"
        "}\n");
    const auto& aliases = schedule(ir, "aliases(bool)");
    expect(has_borrow(aliases, "owner -> observed"),
        "observation-only local alias was not borrowed");
    expect(!has_borrow(aliases, "owner -> uncertain"),
        "branch-sensitive alias was borrowed without a lifetime proof");
    expect(!has_borrow(aliases, "replaced_owner -> stale_candidate"),
        "alias was borrowed across replacement of its owner");
    expect(!has_borrow(aliases, "loop_owner -> loop_candidate"),
        "loop-sensitive alias was borrowed without a lifetime proof");
    expect(!has_borrow(aliases, "moved -> destination"),
        "ownership transfer was also labeled as a borrow");
    expect(std::ranges::any_of(aliases.optimized_operations,
        [](const auto& operation) {
            return operation.kind == toro::IRArcOperationKind::OwnershipTransfer
                && operation.value == "moved -> destination";
        }), "final-use ownership transfer disappeared");
}

void test_field_collection_stack_heap_and_differential_behavior()
{
    constexpr std::string_view source =
        "class Item {\n"
        "    public value: int\n"
        "    destroy() { print(self.value) }\n"
        "}\n"
        "class Holder { public item: Item }\n"
        "function inspect(value: Item) { print(value.value) }\n"
        "function make(value: int) -> Item { return Item(value: value) }\n"
        "function main() {\n"
        "    stack_value := Item(value: 1)\n"
        "    inspect(stack_value)\n"
        "    heap_value := make(2)\n"
        "    inspect(heap_value)\n"
        "    holder := Holder(item: heap_value)\n"
        "    inspect(holder.item)\n"
        "    values := List<Item>(heap_value)\n"
        "    inspect(values[0])\n"
        "}\n";
    const auto enabled = analyze(source, true);
    const auto disabled = analyze(source, false);
    const auto& main_schedule = schedule(enabled, "main()");
    expect(has_borrow(main_schedule, "stack_value -> inspect.value")
            && has_borrow(main_schedule, "heap_value -> inspect.value")
            && has_borrow(main_schedule, "member item -> inspect.value")
            && has_borrow(main_schedule, "collection element -> inspect.value"),
        "stack/heap/field/collection observations were not borrowed");
    expect(enabled.arc_metrics.borrows_inferred > 0
            && enabled.arc_metrics.retains_after
                < disabled.arc_metrics.retains_after
            && enabled.arc_metrics.releases_after
                < disabled.arc_metrics.releases_after,
        "borrowing did not reduce ARC traffic");
    expect(toro::dump_ir(enabled, false, false, true)
            == toro::dump_ir(disabled, false, false, true),
        "borrow inference changed escape or storage decisions");
    expect(toro::dump_ir(enabled, true, false, false)
            == toro::dump_ir(disabled, true, false, false),
        "borrow inference changed effect summaries");
    const auto [enabled_status, enabled_output] = capture_run(
        toro::CGenerator().generate(enabled));
    const auto [disabled_status, disabled_output] = capture_run(
        toro::CGenerator().generate(disabled));
    expect(enabled_status == 0 && disabled_status == 0,
        "borrow differential program failed");
    expect(enabled_output == disabled_output,
        "borrowing changed output or lifecycle behavior");
    expect(enabled_output == "1\n2\n2\n2\n2\n1\n",
        "borrow differential destruction trace was unexpected: "
            + enabled_output);
}

void test_deterministic_inspection()
{
    constexpr std::string_view source =
        "class Box { public value: int }\n"
        "function inspect(value: Box) { print(value.value) }\n"
        "function main() {\n"
        "    value := Box(value: 1)\n"
        "    inspect(value)\n"
        "}\n";
    const auto first = analyze(source);
    const auto second = analyze(source);
    expect(toro::dump_ir(first, false, true)
            == toro::dump_ir(second, false, true),
        "borrow inspection was not deterministic");
}

} // namespace

int main()
{
    try {
        test_parameter_specific_direct_and_forwarded_borrows();
        test_methods_virtual_interfaces_and_generics();
        test_unsafe_dynamic_and_weak_cases_remain_owning();
        test_foreign_and_unknown_call_boundaries_remain_owning();
        test_local_aliases_and_transfer_distinction();
        test_field_collection_stack_heap_and_differential_behavior();
        test_deterministic_inspection();
    } catch (const std::exception& error) {
        std::cerr << "borrow inference test failure: " << error.what() << '\n';
        return 1;
    }
    std::cout << "borrow inference tests passed\n";
    return 0;
}
