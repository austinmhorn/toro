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
        "function validate_bytes() -> Result<bool, string> {\n"
        "    ascii := \"A9 \t!\"\n"
        "    if byte_at(ascii, 0)? != 65 or byte_at(ascii, 1)? != 57 or byte_at(ascii, 2)? != 32 or byte_at(ascii, 3)? != 9 or byte_at(ascii, 4)? != 33 { return ok(false) }\n"
        "    utf8 := \"é\"\n"
        "    return ok(byte_at(utf8, 0)? == 195 and byte_at(utf8, 1)? == 169)\n"
        "}\n"
        "function return_text(value: string) -> string { return value }\n"
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
        "    first := -1\n"
        "    middle := -1\n"
        "    final := -1\n"
        "    handle byte_at(text, 0) {\n        ok(value) { first = value }\n        error(problem) { write_line(problem) }\n    }\n"
        "    handle byte_at(text, 1) {\n        ok(value) { middle = value }\n        error(problem) { write_line(problem) }\n    }\n"
        "    handle byte_at(text, 3) {\n        ok(value) { final = value }\n        error(problem) { write_line(problem) }\n    }\n"
        "    if first != 84 or middle != 111 or final != 111 { return 9 }\n"
        "    boundary_failed := false\n"
        "    handle byte_at(\"\", 0) {\n        ok(value) { first = value }\n        error(problem) { boundary_failed = length(problem) > 0 }\n    }\n"
        "    if boundary_failed == false { return 10 }\n"
        "    boundary_failed = false\n"
        "    handle byte_at(text, 4) {\n        ok(value) { first = value }\n        error(problem) { boundary_failed = length(problem) > 0 }\n    }\n"
        "    if boundary_failed == false { return 11 }\n"
        "    boundary_failed = false\n"
        "    handle byte_at(text, -1) {\n        ok(value) { first = value }\n        error(problem) { boundary_failed = length(problem) > 0 }\n    }\n"
        "    if boundary_failed == false { return 12 }\n"
        "    boundary_failed = false\n"
        "    handle byte_at(text, 99) {\n        ok(value) { first = value }\n        error(problem) { boundary_failed = length(problem) > 0 }\n    }\n"
        "    if boundary_failed == false { return 13 }\n"
        "    bytes_valid := false\n"
        "    handle validate_bytes() {\n        ok(value) { bytes_valid = value }\n        error(problem) { write_line(problem) }\n    }\n"
        "    if bytes_valid == false { return 14 }\n"
        "    if length(\"\") != 0 or length(\"Toro\") != 4 or length(\"é\") != 2 { return 15 }\n"
        "    owned := concat(\"é\", \"!\")\n"
        "    alias := owned\n"
        "    returned := return_text(alias)\n"
        "    if length(owned) != 3 or length(alias) != 3 or length(returned) != 3 { return 16 }\n"
        "    if starts_with(text, \"To\") == false or ends_with(text, \"ro\") == false { return 3 }\n"
        "    piece := \"\"\n"
        "    handle substring(text, 1, 2) {\n"
        "        ok(value) { piece = value }\n"
        "        error(problem) { write_line(problem) }\n"
        "    }\n"
        "    if length(piece) != 2 { return 17 }\n"
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
    const auto byte_at_start = generated.find("toro_std_string_byte_at");
    expect(byte_at_start != std::string::npos,
        "generated C omitted the byte-at helper");
    const auto byte_at_end = generated.find("}\n\n", byte_at_start);
    expect(byte_at_end != std::string::npos,
        "generated byte-at helper was malformed");
    const auto byte_at_helper = generated.substr(
        byte_at_start, byte_at_end - byte_at_start);
    expect(byte_at_helper.find("strlen(") == std::string::npos,
        "generated byte-at helper rescanned string length");
    expect(byte_at_helper.find("toro_length") != std::string::npos,
        "generated byte-at helper did not use stored byte length");
    expect(generated.find(".toro_length") != std::string::npos,
        "generated string length operation did not use descriptor metadata");
    const auto [status, output] = capture_run(generated);
    expect(status == 0,
        "standard-library native program failed with status " + std::to_string(status));
    expect(output == "hello Toro\n", "standard-library console output was incorrect");
    expect(!std::filesystem::exists(data_path),
        "standard-library test did not remove its temporary file");
}

void test_reclaimable_string_lifetimes()
{
    TestTree tree;
    tree.write("main.toro",
        "import strings\n"
        "enum TextValue {\n    text(string)\n    empty\n}\n"
        "struct TextBox { value: string }\n"
        "class TextHolder {\n    public value: string\n}\n"
        "function make(value: string) -> string { return concat(value, \"!\") }\n"
        "function forward(value: string) -> string { return value }\n"
        "function branch(value: string, direct: bool) -> string {\n"
        "    if direct { return value }\n"
        "    return concat(value, \"?\")\n"
        "}\n"
        "function main() -> int {\n"
        "    values := List<string>()\n"
        "    array := Array<string>(\"seed\")\n"
        "    mapped := Map<string, string>()\n"
        "    holder := TextHolder()\n"
        "    value := \"seed\"\n"
        "    index := 0\n"
        "    while index < 20000 {\n"
        "        value = forward(make(\"x\"))\n"
        "        alias := value\n"
        "        if length(alias) != 2 { return 7 }\n"
        "        boxed := TextBox(value: alias)\n"
        "        if length(boxed.value) != 2 { return 8 }\n"
        "        holder.value = boxed.value\n"
        "        if length(holder.value) != 2 { return 9 }\n"
        "        array[0] = holder.value\n"
        "        if length(array[0]) != 2 { return 10 }\n"
        "        mapped[concat(\"k\", \"ey\")] = array[0]\n"
        "        if length(mapped[\"key\"]) != 2 { return 11 }\n"
        "        if mapped.contains(concat(\"k\", \"ey\")) == false { return 6 }\n"
        "        if index < 10000 {\n"
        "            if branch(mapped[concat(\"k\", \"ey\")], true) != \"x!\" { return 4 }\n"
        "        } else {\n"
        "            if branch(mapped[concat(\"k\", \"ey\")], false) != \"x!?\" { return 5 }\n"
        "        }\n"
        "        values.add(value)\n"
        "        if values.count == 64 { values.clear() }\n"
        "        wrapped := TextValue::text(value)\n"
        "        handle wrapped {\n            text(item) { if item != \"x!\" or length(item) != 2 { return 1 } }\n            empty {}\n        }\n"
        "        handle substring(value, 0, 1) {\n            ok(piece) { if piece != \"x\" or length(piece) != 1 { return 2 } }\n            error(problem) { return 3 }\n        }\n"
        "        index = index + 1\n"
        "    }\n"
        "    return 0\n"
        "}\n");
    auto program = load_and_check(tree.path("main.toro"), tree.path(""));
    const std::string generated = toro::CGenerator().generate(program);
    expect(generated.find("atexit") == std::string::npos,
        "native Toro strings still used process-lifetime cleanup");
    expect(generated.find("toro_string_retain") != std::string::npos
            && generated.find("toro_string_release") != std::string::npos,
        "generated C omitted reclaimable string lifetime operations");
    expect(toro::NativeCompiler().run(generated) == 0,
        "repeated reclaimable string lifetime program failed");
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
        test_reclaimable_string_lifetimes();
        test_intrinsics_are_not_source_api();
        test_process_exit_status();
    } catch (const std::exception& error) {
        std::cerr << "standard library test failure: " << error.what() << '\n';
        return 1;
    }
    std::cout << "standard library tests passed\n";
    return 0;
}
