#include "toro/CGenerator.hpp"
#include "toro/Lexer.hpp"
#include "toro/NativeCompiler.hpp"
#include "toro/Parser.hpp"
#include "toro/SemanticAnalyzer.hpp"
#include "toro/TypeChecker.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

void expect(bool condition, std::string_view message)
{
    if (!condition) throw std::runtime_error(std::string(message));
}

std::string generate(std::string_view source)
{
    auto tokens = toro::Lexer(source).tokenize();
    const auto program = toro::Parser(std::move(tokens)).parse_program();
    toro::SemanticAnalyzer().analyze(program);
    toro::TypeChecker().check(program);
    return toro::CGenerator().generate(program);
}

constexpr std::string_view ffi_program =
    "extern \"C\" function puts(text: c_string) -> c_int\n"
    "extern \"C\" function abs(value: c_int) -> c_int\n"
    "extern \"C\" function pow(base: c_double, exponent: c_double) -> c_double\n"
    "extern \"C\" function ffi_types(value: c_int64, flag: c_bool) -> c_bool\n"
    "class Recorder {\n"
    "    public first: dec\n"
    "    public second: dec\n"
    "    private count: int\n"
    "    public function mark(value: dec) -> dec {\n"
    "        self.count = self.count + 1\n"
    "        if self.count == 1 { self.first = value } else { self.second = value }\n"
    "        return value\n"
    "    }\n"
    "}\n"
    "function main() -> int {\n"
    "    puts(\"ffi\" as c_string)\n"
    "    absolute := abs((-7) as c_int) as int\n"
    "    recorder := Recorder()\n"
    "    powered := pow(\n"
    "        recorder.mark(2.0) as c_double,\n"
    "        recorder.mark(3.0) as c_double\n"
    "    ) as dec\n"
    "    if absolute == 7 and powered == 8.0 and recorder.first == 2.0 and recorder.second == 3.0 {\n"
    "        return 0\n"
    "    }\n"
    "    return 1\n"
    "}\n";

void test_external_lowering_and_determinism()
{
    const std::string first = generate(ffi_program);
    const std::string second = generate(ffi_program);
    expect(first == second, "FFI C generation was not deterministic");
    expect(first.find("extern int puts(const char* toro_arg_text);")
            != std::string::npos,
        "C string external prototype was not emitted");
    expect(first.find("extern int abs(int toro_arg_value);")
            != std::string::npos,
        "C integer external prototype was not emitted");
    expect(first.find("extern double pow(double toro_arg_base, double toro_arg_exponent);")
            != std::string::npos,
        "C floating-point external prototype was not emitted");
    expect(first.find("extern bool ffi_types(int64_t toro_arg_value, bool toro_arg_flag);")
            != std::string::npos,
        "fixed-width integer or C bool prototype was not emitted");
    expect(first.find("toro_fn_puts") == std::string::npos,
        "external C symbol was incorrectly Toro-mangled");
    const auto left = first.find("toro_ffi_argument_3");
    const auto right = first.find("toro_ffi_argument_4");
    expect(left != std::string::npos && right != std::string::npos && left < right,
        "external arguments were not materialized left-to-right");
}

void test_native_calls_and_argument_order()
{
    const std::string generated = generate(ffi_program);
    expect(toro::NativeCompiler().run(generated, {"m"}) == 0,
        "native C FFI program failed or evaluated arguments out of order");
}

void test_direct_libc_workflow()
{
    const std::string generated = generate(
        "extern \"C\" function abs(value: c_int) -> c_int\n"
        "function main() -> int {\n"
        "    return abs((-9) as c_int) as int - 9\n"
        "}\n");
    expect(toro::NativeCompiler().run(generated) == 0,
        "direct-file-style libc FFI workflow failed");
}

void test_unresolved_external_reports_native_failure()
{
    const std::string generated = generate(
        "extern \"C\" function phase_7_6_missing_symbol() -> c_int\n"
        "function main() -> int {\n"
        "    return phase_7_6_missing_symbol() as int\n"
        "}\n");
    try {
        static_cast<void>(toro::NativeCompiler().run(generated));
    } catch (const std::exception& error) {
        expect(std::string_view(error.what()).find("C compiler failed")
                != std::string_view::npos,
            "unresolved external symbol did not report a native compiler failure");
        return;
    }
    throw std::runtime_error("unresolved external symbol unexpectedly linked");
}

} // namespace

int main()
{
    try {
        test_external_lowering_and_determinism();
        test_native_calls_and_argument_order();
        test_direct_libc_workflow();
        test_unresolved_external_reports_native_failure();
    } catch (const std::exception& error) {
        std::cerr << "FFI test failure: " << error.what() << '\n';
        return 1;
    }
    std::cout << "FFI tests passed\n";
    return 0;
}
