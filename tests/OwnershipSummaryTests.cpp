#include "toro/ArcOptimizer.hpp"
#include "toro/BorrowInferenceAnalyzer.hpp"
#include "toro/EffectAnalyzer.hpp"
#include "toro/EscapeAnalyzer.hpp"
#include "toro/IRLowerer.hpp"
#include "toro/Lexer.hpp"
#include "toro/OwnershipSummaryAnalyzer.hpp"
#include "toro/Parser.hpp"
#include "toro/SemanticAnalyzer.hpp"
#include "toro/StorageDecisionAnalyzer.hpp"
#include "toro/TypeChecker.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

void expect(bool condition, std::string_view message)
{
    if (!condition) throw std::runtime_error(std::string(message));
}

toro::IRProgram summaries(std::string_view source)
{
    auto program = toro::Parser(toro::Lexer(source).tokenize()).parse_program();
    toro::SemanticAnalyzer().analyze(program);
    toro::TypeChecker().check(program);
    auto ir = toro::IRLowerer().lower(program);
    toro::EffectAnalyzer().analyze(ir);
    toro::OwnershipSummaryAnalyzer().analyze(ir);
    return ir;
}

const toro::IRCallableOwnershipSummary& callable(
    const toro::IRProgram& program,
    std::string_view suffix)
{
    const auto found = std::ranges::find_if(program.ownership_summaries,
        [&](const auto& summary) {
            return summary.callable_identity.ends_with(suffix);
        });
    if (found == program.ownership_summaries.end()) {
        throw std::runtime_error("missing ownership summary: "
            + std::string(suffix));
    }
    return *found;
}

const toro::IRParameterOwnershipSummary& parameter(
    const toro::IRProgram& program,
    std::string_view callable_suffix,
    std::string_view name)
{
    const auto& owner = callable(program, callable_suffix);
    const auto found = std::ranges::find_if(owner.parameters,
        [&](const auto& candidate) {
            return candidate.parameter_name == name;
        });
    if (found == owner.parameters.end()) {
        throw std::runtime_error("missing ownership parameter: "
            + std::string(name));
    }
    return *found;
}

bool has(
    const toro::IRParameterOwnershipSummary& summary,
    toro::IROwnershipBehavior behavior)
{
    return std::ranges::find(summary.behaviors, behavior)
        != summary.behaviors.end();
}

void test_direct_chains_recursion_and_parameter_precision()
{
    const auto ir = summaries(
        "class Box { public value: int }\n"
        "class Sink { public value: Box? }\n"
        "function inspect(value: Box) { print(value.value) }\n"
        "function wrapper(value: Box) { inspect(value) }\n"
        "function outer(value: Box) { wrapper(value) }\n"
        "function store(sink: Sink, value: Box) { sink.value = value }\n"
        "function store_wrapper(sink: Sink, value: Box) { store(sink, value) }\n"
        "function split(sink: Sink, observed: Box, escaped: Box) {\n"
        "    print(observed.value)\n"
        "    sink.value = escaped\n"
        "}\n"
        "function recurse(value: Box, count: int) {\n"
        "    if count > 0 { recurse(value, count - 1) }\n"
        "    else { print(value.value) }\n"
        "}\n"
        "function first(value: Box, flag: bool) {\n"
        "    if flag { second(value, false) } else { print(value.value) }\n"
        "}\n"
        "function second(value: Box, flag: bool) {\n"
        "    if flag { first(value, false) } else { print(value.value) }\n"
        "}\n");
    expect(toro::ownership_is_observed_only(parameter(ir, "inspect(Box)", "value"))
            && toro::ownership_is_observed_only(parameter(ir, "wrapper(Box)", "value"))
            && toro::ownership_is_observed_only(parameter(ir, "outer(Box)", "value")),
        "observation-only wrapper chain did not remain borrow-safe");
    const auto& forwarded = parameter(ir, "store_wrapper(Sink,Box)", "value");
    expect(has(forwarded,
               toro::IROwnershipBehavior::Store)
            && has(forwarded,
                toro::IROwnershipBehavior::Escape),
        "persistent storage did not propagate through a wrapper");
    expect(std::ranges::any_of(forwarded.causes,
            [](const auto& cause) {
                return cause.transitive
                    && cause.via_callable.ends_with("store(Sink,Box)");
            }),
        "transitive ownership cause did not retain its callee identity");
    expect(toro::ownership_is_observed_only(
            parameter(ir, "split(Sink,Box,Box)", "observed"))
            && has(parameter(ir, "split(Sink,Box,Box)", "escaped"),
                toro::IROwnershipBehavior::Escape),
        "one escaping parameter contaminated an unrelated parameter");
    expect(toro::ownership_is_observed_only(parameter(ir,
               "recurse(Box,int)", "value"))
            && toro::ownership_is_observed_only(parameter(ir,
                "first(Box,bool)", "value"))
            && toro::ownership_is_observed_only(parameter(ir,
                "second(Box,bool)", "value")),
        "recursive fixed-point analysis did not converge to observation-only");
}

void test_returns_fields_collections_and_weak_storage()
{
    const auto ir = summaries(
        "class Box { public value: int }\n"
        "class Holder { public value: Box }\n"
        "class WeakHolder { public weak value: Box? }\n"
        "function same_wrapper(value: Box) -> Box { return same(value) }\n"
        "function make_wrapper() -> Box { return make() }\n"
        "function same(value: Box) -> Box { return value }\n"
        "function make() -> Box { return Box(value: 1) }\n"
        "function lookup(holder: Holder) -> Box { return holder.value }\n"
        "function wrap(value: Box) -> Result<Box, string> { return ok(value) }\n"
        "function strong(holder: Holder, value: Box) { holder.value = value }\n"
        "function weak_store(holder: WeakHolder, value: Box) { holder.value = value }\n"
        "function collect(values: List<Box>, value: Box) { values.add(value) }\n");
    const auto& same = callable(ir, "same(Box)");
    expect(same.return_value.ownership
            == toro::IRReturnOwnership::ParameterAlias
            && has(parameter(ir, "same(Box)", "value"),
                toro::IROwnershipBehavior::Return),
        "returned input relationship was not summarized");
    expect(callable(ir, "same_wrapper(Box)").return_value.ownership
            == toro::IRReturnOwnership::ParameterAlias
            && callable(ir, "make_wrapper()").return_value.ownership
                == toro::IRReturnOwnership::NewOwned,
        "managed return relationships did not propagate through forward calls");
    expect(callable(ir, "make()").return_value.ownership
            == toro::IRReturnOwnership::NewOwned,
        "new owning return was not summarized");
    expect(callable(ir, "lookup(Holder)").return_value.ownership
            == toro::IRReturnOwnership::FieldAlias
            && toro::ownership_is_observed_only(
                parameter(ir, "lookup(Holder)", "holder")),
        "managed field return relationship was not summarized");
    expect(has(parameter(ir, "wrap(Box)", "value"),
               toro::IROwnershipBehavior::Return)
            && has(parameter(ir, "wrap(Box)", "value"),
                toro::IROwnershipBehavior::Escape),
        "managed Result payload return was not summarized as escaping");
    expect(has(parameter(ir, "strong(Holder,Box)", "value"),
               toro::IROwnershipBehavior::Store)
            && has(parameter(ir, "collect(List<Box>,Box)", "value"),
                toro::IROwnershipBehavior::Store),
        "strong field or collection storage was not summarized");
    const auto& weak_value = parameter(ir, "weak_store(WeakHolder,Box)", "value");
    expect(has(weak_value, toro::IROwnershipBehavior::WeakStore)
            && !has(weak_value, toro::IROwnershipBehavior::Retain)
            && !has(weak_value, toro::IROwnershipBehavior::Escape),
        "weak storage was treated as ownership retention");
}

void test_methods_virtual_interfaces_and_inheritance()
{
    const auto ir = summaries(
        "class Box { public value: int }\n"
        "class Sink { public value: Box? }\n"
        "class SafeBase {\n"
        "    public virtual function use(value: Box) { print(value.value) }\n"
        "    public function mutate() {\n"
        "        self_count := 1\n"
        "        print(self_count)\n"
        "    }\n"
        "}\n"
        "class SafeDerived : SafeBase {\n"
        "    public override function use(value: Box) { print(value.value) }\n"
        "}\n"
        "class MixedBase { public virtual function use(value: Box, sink: Sink) { print(value.value) } }\n"
        "class MixedDerived : MixedBase {\n"
        "    public override function use(value: Box, sink: Sink) { sink.value = value }\n"
        "}\n"
        "interface Reader { function read(value: Box, sink: Sink) }\n"
        "class ReaderA implements Reader { public function read(value: Box, sink: Sink) { print(value.value) } }\n"
        "class ReaderB implements Reader { public function read(value: Box, sink: Sink) { print(value.value) } }\n"
        "interface MixedReader { function read(value: Box, sink: Sink) }\n"
        "class MixedA implements MixedReader { public function read(value: Box, sink: Sink) { print(value.value) } }\n"
        "class MixedB implements MixedReader { public function read(value: Box, sink: Sink) { sink.value = value } }\n"
        "interface Named { function name() -> int }\n"
        "class NamedBase { public function name() -> int { return 1 } }\n"
        "class NamedDerived : NamedBase implements Named { }\n"
        "function safe_virtual(target: SafeBase, value: Box) { target.use(value) }\n"
        "function mixed_virtual(target: MixedBase, value: Box, sink: Sink) { target.use(value, sink) }\n"
        "function safe_interface(target: Reader, value: Box, sink: Sink) { target.read(value, sink) }\n"
        "function mixed_interface(target: MixedReader, value: Box, sink: Sink) { target.read(value, sink) }\n"
        "function inherited(target: Named) { print(target.name()) }\n");
    expect(toro::ownership_is_observed_only(
            parameter(ir, "SafeBase::use(Box)", "self"))
            && toro::ownership_is_observed_only(
                parameter(ir, "SafeBase::use(Box)", "value")),
        "ordinary method receiver or parameter was not observation-only");
    expect(toro::ownership_is_observed_only(
            parameter(ir, "safe_virtual(SafeBase,Box)", "value")),
        "all-safe virtual target set was not joined precisely");
    expect(has(parameter(ir, "mixed_virtual(MixedBase,Box,Sink)", "value"),
            toro::IROwnershipBehavior::Escape),
        "unsafe virtual target did not make the joined argument escape");
    expect(toro::ownership_is_observed_only(
            parameter(ir, "safe_interface(Reader,Box,Sink)", "value")),
        "all-safe interface implementations were not joined precisely");
    expect(has(parameter(ir, "mixed_interface(MixedReader,Box,Sink)", "value"),
            toro::IROwnershipBehavior::Escape),
        "unsafe interface implementation did not make the joined argument escape");
    expect(toro::ownership_is_observed_only(
            parameter(ir, "inherited(Named)", "target")),
        "inherited interface implementation was not resolved safely");
}

void test_generics_foreign_unknown_and_determinism()
{
    constexpr std::string_view source =
        "extern \"C\" function puts(text: c_string) -> c_int\n"
        "class Box { public value: int }\n"
        "class Tools { public function inspect<T>(value: T) { } }\n"
        "function inspect<T>(value: T) { }\n"
        "function calls(tools: Tools, value: Box) {\n"
        "    inspect<int>(1)\n"
        "    inspect<Box>(value)\n"
        "    tools.inspect<Box>(value)\n"
        "    puts(\"effect\" as c_string)\n"
        "    print(value.value)\n"
        "}\n";
    const auto first = summaries(source);
    const auto second = summaries(source);
    const auto& integer = callable(first, "inspect(T)<T=int>");
    const auto& managed = callable(first, "inspect(T)<T=Box>");
    expect(integer.parameters.empty(),
        "non-managed generic specialization gained ownership facts");
    expect(managed.parameters.size() == 1
            && toro::ownership_is_observed_only(managed.parameters.front()),
        "managed generic specialization lost its concrete ownership fact");
    expect(toro::ownership_is_observed_only(
            parameter(first, "Tools::inspect(T)<T=Box>", "value")),
        "generic method specialization was not summarized");
    expect(toro::ownership_is_observed_only(
            parameter(first, "calls(Tools,Box)", "value")),
        "unrelated foreign/stdlib-style intrinsic effects contaminated a parameter");
    expect(toro::dump_ir(first, false, false, false, true)
            == toro::dump_ir(second, false, false, false, true),
        "ownership summary output was not deterministic");
}

void test_cross_pass_escape_borrow_storage_and_arc()
{
    auto ir = summaries(
        "class Box { public value: int }\n"
        "class Sink { public value: Box? }\n"
        "function inspect(value: Box) { print(value.value) }\n"
        "function wrapper(value: Box) { inspect(value) }\n"
        "function store(sink: Sink, value: Box) { sink.value = value }\n"
        "function safe() {\n"
        "    value := Box(value: 1)\n"
        "    wrapper(value)\n"
        "}\n"
        "function escaping(sink: Sink) {\n"
        "    value := Box(value: 2)\n"
        "    store(sink, value)\n"
        "}\n");
    toro::EscapeAnalyzer().analyze(ir);
    toro::StorageDecisionAnalyzer().analyze(ir);
    const auto safe = std::ranges::find_if(ir.escape_allocations,
        [](const auto& allocation) {
            return allocation.callable_identity.ends_with("safe()");
        });
    const auto escaping = std::ranges::find_if(ir.escape_allocations,
        [](const auto& allocation) {
            return allocation.callable_identity.ends_with("escaping(Sink)");
        });
    expect(safe != ir.escape_allocations.end()
            && safe->state == toro::IREscapeState::DoesNotEscape
            && safe->storage == toro::IRStorageClass::Stack,
        "safe wrapper chain did not preserve local stack storage");
    expect(escaping != ir.escape_allocations.end()
            && escaping->state == toro::IREscapeState::Escapes
            && escaping->storage == toro::IRStorageClass::Heap,
        "escaping wrapper chain did not preserve heap storage");
    toro::ArcMaterializer().materialize(ir);
    toro::OwnershipTransferAnalyzer().analyze(ir);
    toro::BorrowInferenceAnalyzer().analyze(ir);
    toro::ArcOptimizer().optimize(ir);
    expect(ir.arc_metrics.borrows_inferred > 0,
        "summary-proven wrapper chain did not produce borrow facts");
}

} // namespace

int main()
{
    try {
        test_direct_chains_recursion_and_parameter_precision();
        test_returns_fields_collections_and_weak_storage();
        test_methods_virtual_interfaces_and_inheritance();
        test_generics_foreign_unknown_and_determinism();
        test_cross_pass_escape_borrow_storage_and_arc();
    } catch (const std::exception& error) {
        std::cerr << "ownership summary test failure: " << error.what() << '\n';
        return 1;
    }
    std::cout << "ownership summary tests passed\n";
    return 0;
}
