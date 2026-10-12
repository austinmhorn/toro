#include "toro/CGenerator.hpp"
#include "toro/ModuleLoader.hpp"
#include "toro/NativeCompiler.hpp"
#include "toro/SemanticAnalyzer.hpp"
#include "toro/TypeChecker.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace {

void expect(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

class TestTree {
public:
    TestTree()
    {
#if defined(_WIN32)
        root_ = std::filesystem::temp_directory_path() / "toro-stdlib-tests";
#else
        root_ = std::filesystem::temp_directory_path()
            / ("toro-stdlib-tests-" + std::to_string(::getpid()));
#endif
        std::filesystem::remove_all(root_);
        std::filesystem::create_directories(root_);
    }

    ~TestTree() { std::filesystem::remove_all(root_); }

    void write(std::string_view relative, std::string_view contents) const
    {
        const auto path = root_ / relative;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream output(path);
        if (!output) {
            throw std::runtime_error("could not create standard-library test file");
        }
        output << contents;
    }

    [[nodiscard]] std::filesystem::path path(std::string_view relative) const
    {
        return root_ / relative;
    }

private:
    std::filesystem::path root_;
};

toro::Program load_and_check(
    const std::filesystem::path& entry,
    const std::filesystem::path& root)
{
    auto program = toro::ModuleLoader().load(entry, root);
    toro::SemanticAnalyzer().analyze(program);
    toro::TypeChecker().check(program);
    return program;
}

std::pair<int, std::string> capture_run(std::string_view c_source)
{
#if defined(_WIN32)
    static_cast<void>(c_source);
    throw std::runtime_error("standard-library native tests require a POSIX host");
#else
    int output_pipe[2];
    if (::pipe(output_pipe) != 0) {
        throw std::runtime_error("could not create standard-library output pipe");
    }
    std::fflush(stdout);
    const int saved_stdout = ::dup(STDOUT_FILENO);
    if (saved_stdout == -1 || ::dup2(output_pipe[1], STDOUT_FILENO) == -1) {
        throw std::runtime_error("could not redirect standard-library output");
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
    return {status, std::move(output)};
#endif
}

void test_resolution_precedence_and_determinism()
{
    TestTree first;
    first.write("io.toro", "this is not valid Toro source\n");
    first.write("main.toro",
        "import io\n"
        "function main() { write_line(\"resolved\") }\n");
    auto program = load_and_check(first.path("main.toro"), first.path(""));
    expect(program.modules.size() == 2,
        "standard-library import did not produce one shared module");
    expect(program.modules.front().name == "io",
        "standard-library module identity was not stable");
    expect(program.modules.front().path == "<toro-stdlib>/io.toro",
        "project module unexpectedly shadowed the standard library");
    const std::string generated = toro::CGenerator().generate(program);

    TestTree second;
    second.write("main.toro",
        "import io\n"
        "function main() { write_line(\"resolved\") }\n");
    auto repeated = load_and_check(second.path("main.toro"), second.path(""));
    expect(generated == toro::CGenerator().generate(repeated),
        "standard-library C output depended on the project path");

    TestTree reserved;
    reserved.write("io.toro", "function main() {}\n");
    try {
        static_cast<void>(toro::ModuleLoader().load(
            reserved.path("io.toro"), reserved.path("")));
    } catch (const std::exception& error) {
        expect(std::string_view(error.what()).find("reserved by the Toro standard library")
                != std::string_view::npos,
            "reserved standard-library entry name produced an unclear diagnostic");
        return;
    }
    throw std::runtime_error("reserved standard-library module name was accepted as entry");
}

void test_native_standard_library()
{
    TestTree tree;
    const std::string data_path = tree.path("data.txt").string();
    tree.write("main.toro",
        "import env\n"
        "import fs\n"
        "import io\n"
        "import strings\n"
        "import time\n"
        "class Recorder {\n"
        "    public first: string\n"
        "    public second: string\n"
        "    private count: int\n"
        "    public function mark(value: string) -> string {\n"
        "        self.count = self.count + 1\n"
        "        if self.count == 1 { self.first = value } else { self.second = value }\n"
        "        return value\n"
        "    }\n"
        "}\n"
        "function main() -> int {\n"
        "    path := \"" + data_path + "\"\n"
        "    wrote := false\n"
        "    handle write_text(path, \"Toro\") {\n"
        "        ok(value) { wrote = value }\n"
        "        error(problem) { write_line(problem) }\n"
        "    }\n"
        "    if wrote == false or exists(path) == false { return 1 }\n"
        "    text := \"\"\n"
        "    handle read_text(path) {\n"
        "        ok(value) { text = value }\n"
        "        error(problem) { write_line(problem) }\n"
        "    }\n"
        "    if length(text) != 4 or contains(text, \"or\") == false { return 2 }\n"
        "    if starts_with(text, \"To\") == false or ends_with(text, \"ro\") == false { return 3 }\n"
        "    piece := \"\"\n"
        "    handle substring(text, 1, 2) {\n"
        "        ok(value) { piece = value }\n"
        "        error(problem) { write_line(problem) }\n"
        "    }\n"
        "    write_line(concat(\"hello \", text))\n"
        "    if piece != \"or\" or has(\"PATH\") == false { return 4 }\n"
        "    has_path := false\n"
        "    handle get(\"PATH\") {\n"
        "        ok(value) { has_path = length(value) > 0 }\n"
        "        error(problem) { write_line(problem) }\n"
        "    }\n"
        "    started := wall_millis()\n"
        "    if has_path == false or started <= 0 or elapsed_millis(started) < 0 { return 5 }\n"
        "    handle remove_file(path) {\n"
        "        ok(value) { wrote = value }\n"
        "        error(problem) { write_line(problem) }\n"
        "    }\n"
        "    if wrote == false or exists(path) { return 6 }\n"
        "    missing_failed := false\n"
        "    handle read_text(path) {\n"
        "        ok(value) { text = value }\n"
        "        error(problem) { missing_failed = length(problem) > 0 }\n"
        "    }\n"
        "    if missing_failed == false { return 7 }\n"
        "    recorder := Recorder()\n"
        "    combined := concat(recorder.mark(\"L\"), recorder.mark(\"R\"))\n"
        "    if combined != \"LR\" or recorder.first != \"L\" or recorder.second != \"R\" { return 8 }\n"
        "    return 0\n"
        "}\n");
    auto program = load_and_check(tree.path("main.toro"), tree.path(""));
    const std::string generated = toro::CGenerator().generate(program);
    const auto [status, output] = capture_run(generated);
    expect(status == 0, "standard-library native program failed");
    expect(output == "hello Toro\n", "standard-library console output was incorrect");
    expect(!std::filesystem::exists(data_path),
        "standard-library test did not remove its temporary file");
}

void test_intrinsics_are_not_source_api()
{
    TestTree tree;
    tree.write("main.toro",
        "function main() { __toro_io_write_line(\"hidden\") }\n");
    try {
        static_cast<void>(load_and_check(tree.path("main.toro"), tree.path("")));
    } catch (const std::exception& error) {
        expect(std::string_view(error.what()).find("unknown identifier")
                != std::string_view::npos,
            "reserved runtime intrinsic produced an unclear diagnostic");
        return;
    }
    throw std::runtime_error("runtime intrinsic was exposed as source API");
}

void test_process_exit_status()
{
    TestTree tree;
    tree.write("main.toro",
        "import process\n"
        "function main() { exit(7) }\n");
    auto program = load_and_check(tree.path("main.toro"), tree.path(""));
    expect(toro::NativeCompiler().run(toro::CGenerator().generate(program)) == 7,
        "process.exit did not preserve the requested native status");
}

} // namespace

int main()
{
    try {
        test_resolution_precedence_and_determinism();
        test_native_standard_library();
        test_intrinsics_are_not_source_api();
        test_process_exit_status();
    } catch (const std::exception& error) {
        std::cerr << "standard library test failure: " << error.what() << '\n';
        return 1;
    }
    std::cout << "standard library tests passed\n";
    return 0;
}
