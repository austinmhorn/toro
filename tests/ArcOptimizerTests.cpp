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
    if (optimize) toro::ArcOptimizer().optimize(ir);
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

} // namespace

int main()
{
    try {
        test_materialization_and_local_parameter_elision();
        test_escape_dynamic_generic_and_storage_barriers();
        test_nullable_observation_and_foreign_barrier();
        test_weak_fields_and_managed_payloads_are_explicit();
        test_backend_consumes_arc_schedule_and_differential_lifecycle();
    } catch (const std::exception& error) {
        std::cerr << "ARC optimizer test failure: " << error.what() << '\n';
        return 1;
    }
    std::cout << "ARC optimizer tests passed\n";
    return 0;
}
