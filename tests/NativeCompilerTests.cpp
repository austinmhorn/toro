#include "toro/CGenerator.hpp"
#include "toro/Lexer.hpp"
#include "toro/NativeCompiler.hpp"
#include "toro/Parser.hpp"
#include "toro/SemanticAnalyzer.hpp"
#include "toro/TypeChecker.hpp"

#include <cstdio>
#include <exception>
#include <filesystem>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#if defined(_WIN32)
#else
#include <unistd.h>
#endif

namespace {

std::string generate(std::string_view source)
{
    auto tokens = toro::Lexer(source).tokenize();
    const auto program = toro::Parser(std::move(tokens)).parse_program();
    toro::SemanticAnalyzer().analyze(program);
    toro::TypeChecker().check(program);
    return toro::CGenerator().generate(program);
}

void expect(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

std::filesystem::path make_test_directory()
{
#if defined(_WIN32)
    throw std::runtime_error("native compiler tests require a POSIX host");
#else
    const auto path = std::filesystem::temp_directory_path()
        / ("toro-native-tests-" + std::to_string(::getpid()));
    std::filesystem::create_directory(path);
    return path;
#endif
}

std::set<std::filesystem::path> native_temporary_directories()
{
    std::set<std::filesystem::path> result;
    for (const auto& entry : std::filesystem::directory_iterator(
             std::filesystem::temp_directory_path())) {
        if (entry.is_directory()
            && entry.path().filename().string().starts_with("toro-native-")) {
            result.insert(entry.path());
        }
    }
    return result;
}

std::pair<int, std::string> capture_run(
    const toro::NativeCompiler& compiler,
    std::string_view c_source)
{
#if defined(_WIN32)
    static_cast<void>(compiler);
    static_cast<void>(c_source);
    throw std::runtime_error("native compiler tests require a POSIX host");
#else
    int output_pipe[2];
    if (::pipe(output_pipe) != 0) {
        throw std::runtime_error("could not create output capture pipe");
    }
    std::fflush(stdout);
    const int saved_stdout = ::dup(STDOUT_FILENO);
    if (saved_stdout == -1 || ::dup2(output_pipe[1], STDOUT_FILENO) == -1) {
        ::close(output_pipe[0]);
        ::close(output_pipe[1]);
        throw std::runtime_error("could not redirect output for native test");
    }
    ::close(output_pipe[1]);

    int status = 0;
    try {
        status = compiler.run(c_source);
    } catch (...) {
        std::fflush(stdout);
        static_cast<void>(::dup2(saved_stdout, STDOUT_FILENO));
        ::close(saved_stdout);
        ::close(output_pipe[0]);
        throw;
    }
    std::fflush(stdout);
    if (::dup2(saved_stdout, STDOUT_FILENO) == -1) {
        ::close(saved_stdout);
        ::close(output_pipe[0]);
        throw std::runtime_error("could not restore output after native test");
    }
    ::close(saved_stdout);

    std::string output;
    char buffer[256];
    ssize_t count;
    while ((count = ::read(output_pipe[0], buffer, sizeof(buffer))) > 0) {
        output.append(buffer, static_cast<std::size_t>(count));
    }
    ::close(output_pipe[0]);
    return {status, std::move(output)};
#endif
}

void test_build_executable()
{
    const auto directory = make_test_directory();
    const auto executable = directory / "hello";
    toro::NativeCompiler().build(
        generate(
            "function main() {\n"
            "    print(\"Hello, toro!\")\n"
            "}\n"),
        executable);
    expect(std::filesystem::is_regular_file(executable), "native executable was not created");
    std::filesystem::remove_all(directory);
}

void test_run_output_and_control_flow()
{
    const auto c_source = generate(
        "function add(left: int, right: int) -> int {\n"
        "    return left + right\n"
        "}\n"
        "function main() {\n"
        "    value := add(3, 4)\n"
        "    text := \"toro\"\n"
        "    if text == \"toro\" and value == 7 {\n"
        "        print(value)\n"
        "    } else {\n"
        "        print(0)\n"
        "    }\n"
        "    while value < 8 {\n"
        "        value = value + 1\n"
        "    }\n"
        "    print(value)\n"
        "    print(true)\n"
        "}\n");
    const auto [status, output] = capture_run(toro::NativeCompiler(), c_source);
    expect(status == 0, "generated program did not exit successfully");
    expect(output == "7\n8\ntrue\n", "generated program produced unexpected output");
}

void test_nonzero_exit_status()
{
    const auto c_source = generate(
        "function main() -> int {\n"
        "    return 7\n"
        "}\n");
    expect(toro::NativeCompiler().run(c_source) == 7, "program exit status was not forwarded");
}

void test_struct_runtime_behavior()
{
    const auto c_source = generate(
        "struct Point {\n"
        "    x: dec\n"
        "    y: dec\n"
        "}\n"
        "struct Player {\n"
        "    name: string\n"
        "    age: int = 28\n"
        "    active: bool\n"
        "    position: Point\n"
        "    function rename(name: string) { self.name = name }\n"
        "    function birthday() { self.age = self.age + 1 }\n"
        "    function get_age() -> int { return self.age }\n"
        "}\n"
        "function main() {\n"
        "    player := Player(\"Austin\", position: Point(10.0, 20.0))\n"
        "    copy := player\n"
        "    copy.age = 99\n"
        "    player.rename(\"toro\")\n"
        "    player.birthday()\n"
        "    player.position.x = 15.0\n"
        "    print(player.name)\n"
        "    print(player.get_age())\n"
        "    print(player.active)\n"
        "    print(player.position.x)\n"
        "    print(copy.age)\n"
        "}\n");
    const auto [status, output] = capture_run(toro::NativeCompiler(), c_source);
    expect(status == 0, "generated struct program did not exit successfully");
    expect(
        output == "toro\n29\nfalse\n15\n99\n",
        "generated struct program produced unexpected output");
}

void test_enum_runtime_behavior()
{
    const auto c_source = generate(
        "struct Point { x: int }\n"
        "enum Inner {\n"
        "    number(int)\n"
        "    text(string)\n"
        "    none\n"
        "}\n"
        "enum Outer {\n"
        "    nested(Inner)\n"
        "    point(Point)\n"
        "    done\n"
        "}\n"
        "function number_or_zero(value: Inner) -> int {\n"
        "    handle value {\n"
        "        number(number) { return number }\n"
        "        text(text) { return 0 }\n"
        "        none { return 0 }\n"
        "    }\n"
        "}\n"
        "function make() -> Outer {\n"
        "    return Outer::nested(Inner::number(7))\n"
        "}\n"
        "function show(value: Outer) {\n"
        "    handle value {\n"
        "        nested(inner) {\n"
        "            handle inner {\n"
        "                number(number) { print(number) }\n"
        "                text(text) { print(text) }\n"
        "                none { print(\"none\") }\n"
        "            }\n"
        "        }\n"
        "        point(point) { print(point.x) }\n"
        "        done { print(\"done\") }\n"
        "    }\n"
        "}\n"
        "function main() {\n"
        "    value := make()\n"
        "    copy := value\n"
        "    show(copy)\n"
        "    copy = Outer::point(Point(3))\n"
        "    show(copy)\n"
        "    show(Outer::done)\n"
        "    print(number_or_zero(Inner::number(8)))\n"
        "}\n");
    const auto [status, output] = capture_run(toro::NativeCompiler(), c_source);
    expect(status == 0, "generated enum program did not exit successfully");
    expect(
        output == "7\n3\ndone\n8\n",
        "generated enum program produced unexpected output");
}

void test_compile_failure_reporting()
{
    const auto directory = make_test_directory();
    const auto executable = directory / "broken";
    try {
        toro::NativeCompiler().build("this is not valid C\n", executable);
    } catch (const std::runtime_error& error) {
        std::filesystem::remove_all(directory);
        expect(
            std::string_view(error.what()).find("C compiler failed")
                != std::string_view::npos,
            "native compiler did not distinguish a C compiler failure");
        return;
    }
    std::filesystem::remove_all(directory);
    throw std::runtime_error("expected native C compilation failure");
}

void test_unsupported_backend_feature()
{
    try {
        static_cast<void>(generate(
            "class Unsupported {\n"
            "}\n"));
    } catch (const std::runtime_error& error) {
        expect(
            std::string_view(error.what()).find("backend error")
                != std::string_view::npos,
            "unsupported feature did not produce a backend diagnostic");
        return;
    }
    throw std::runtime_error("expected unsupported backend feature failure");
}

void test_temporary_cleanup()
{
    const auto before = native_temporary_directories();
    const auto result = toro::NativeCompiler().run(generate(
        "function main() {\n"
        "}\n"));
    const auto after = native_temporary_directories();
    expect(result == 0, "cleanup test program failed");
    expect(before == after, "native run left temporary artifacts behind");
}

} // namespace

int main()
{
    try {
        test_build_executable();
        test_run_output_and_control_flow();
        test_nonzero_exit_status();
        test_struct_runtime_behavior();
        test_enum_runtime_behavior();
        test_compile_failure_reporting();
        test_unsupported_backend_feature();
        test_temporary_cleanup();
    } catch (const std::exception& error) {
        std::cerr << "native compiler test failure: " << error.what() << '\n';
        return 1;
    }

    std::cout << "native compiler tests passed\n";
    return 0;
}
