#include "toro/CGenerator.hpp"
#include "toro/Lexer.hpp"
#include "toro/ModuleLoader.hpp"
#include "toro/NativeCompiler.hpp"
#include "toro/Parser.hpp"
#include "toro/SemanticAnalyzer.hpp"
#include "toro/Token.hpp"
#include "toro/TypeChecker.hpp"

#include <exception>
#include <filesystem>
#include <fstream>
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
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

class TestTree {
public:
    TestTree()
    {
#if defined(_WIN32)
        root_ = std::filesystem::temp_directory_path() / "toro-module-tests";
#else
        root_ = std::filesystem::temp_directory_path()
            / ("toro-module-tests-" + std::to_string(::getpid()));
#endif
        std::filesystem::remove_all(root_);
        std::filesystem::create_directories(root_);
    }

    ~TestTree()
    {
        std::filesystem::remove_all(root_);
    }

    void write(std::string_view relative_path, std::string_view source) const
    {
        const auto path = root_ / relative_path;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream output(path);
        if (!output) {
            throw std::runtime_error("could not create module test source");
        }
        output << source;
    }

    [[nodiscard]] std::filesystem::path path(std::string_view relative) const
    {
        return root_ / relative;
    }

private:
    std::filesystem::path root_;
};

toro::Program load_and_check(const std::filesystem::path& entry)
{
    auto program = toro::ModuleLoader().load(entry);
    toro::SemanticAnalyzer().analyze(program);
    toro::TypeChecker().check(program);
    return program;
}

void expect_failure(
    const std::filesystem::path& entry,
    std::string_view expected)
{
    try {
        static_cast<void>(load_and_check(entry));
    } catch (const std::exception& error) {
        expect(
            std::string_view(error.what()).find(expected) != std::string_view::npos,
            std::string("expected error containing '") + std::string(expected)
                + "', got '" + error.what() + "'");
        return;
    }
    throw std::runtime_error("expected module compilation failure");
}

void test_import_lexing_and_ast()
{
    const auto tokens = toro::Lexer("import game.player\n").tokenize();
    expect(tokens.front().type == toro::TokenType::Import,
        "import was not lexed as a keyword");

    auto program = toro::Parser(
        toro::Lexer("import game.player\npublic function run() {}\n")
            .tokenize()).parse_program();
    expect(program.statements.size() == 2, "unexpected import AST size");
    expect(program.statements[0]->kind == toro::StmtKind::ImportDeclaration,
        "import AST node was not retained");
    const auto& import = static_cast<const toro::ImportDeclarationStmt&>(
        *program.statements[0]);
    expect(import.module_path() == "game.player", "nested module path was not retained");
    expect(program.statements[1]->is_public,
        "public top-level declaration was not marked as exported");
}

void test_loading_visibility_diamond_and_runtime()
{
    TestTree tree;
    tree.write("shared/base.toro",
        "public function shared(value: int) -> int { return value + 1 }\n");
    tree.write("left.toro",
        "import shared.base\n"
        "function adjust(value: int) -> int { return shared(value) }\n"
        "public function left(value: int) -> int { return adjust(value) }\n");
    tree.write("right.toro",
        "import shared.base\n"
        "function adjust(value: int) -> int { return shared(value) }\n"
        "public function right(value: int) -> int { return adjust(value) }\n");
    tree.write("game/player.toro",
        "function private_bonus() -> int { return 1 }\n"
        "public enum Rank {\n"
        "    novice\n"
        "    veteran\n"
        "}\n"
        "public struct Stats { score: int }\n"
        "public class Player {\n"
        "    public name: string\n"
        "    public score: int\n"
        "    public function total() -> int { return self.score + private_bonus() }\n"
        "}\n"
        "public function identity<T>(value: T) -> T { return value }\n"
        "public function describe(value: int) -> string { return \"int\" }\n"
        "public function describe(value: string) -> string { return value }\n");
    tree.write("main.toro",
        "import game.player\n"
        "import game.player\n"
        "import left\n"
        "import right\n"
        "function main() -> int {\n"
        "    player := Player(name: identity<string>(\"Toro\"), score: 40)\n"
        "    stats := Stats(score: player.total())\n"
        "    rank := Rank::veteran\n"
        "    if describe(stats.score) == \"int\" and describe(player.name) == \"Toro\" {\n"
        "        if left(stats.score) == 42 and right(stats.score) == 42 {\n"
        "            handle rank {\n"
        "                novice { return 1 }\n"
        "                veteran { return 0 }\n"
        "            }\n"
        "        }\n"
        "    }\n"
        "    return 1\n"
        "}\n");

    auto program = load_and_check(tree.path("main.toro"));
    expect(program.modules.size() == 5,
        "duplicate/diamond imports did not load each module exactly once");
    std::size_t shared_declarations = 0;
    for (const auto& statement : program.statements) {
        if (statement->kind == toro::StmtKind::FunctionDeclaration
            && static_cast<const toro::FunctionDeclarationStmt&>(*statement).name
                == "shared") {
            ++shared_declarations;
        }
    }
    expect(shared_declarations == 1,
        "diamond import duplicated a shared declaration");
    const std::string first = toro::CGenerator().generate(program);
    auto repeated = load_and_check(tree.path("main.toro"));
    const std::string second = toro::CGenerator().generate(repeated);
    expect(first == second, "multi-module generated C was not deterministic");
    expect(toro::NativeCompiler().run(first) == 0,
        "multi-module native program returned failure");
}

void test_private_visibility()
{
    TestTree tree;
    tree.write("helper.toro",
        "class Hidden {}\n"
        "function hidden() -> int { return 1 }\n"
        "public function visible() -> int { return hidden() }\n");
    tree.write("main.toro",
        "import helper\n"
        "function main() { print(hidden()) }\n");
    expect_failure(tree.path("main.toro"), "unknown identifier 'hidden'");

    tree.write("main.toro",
        "import helper\n"
        "function expose(value: Hidden) {}\n"
        "function main() {}\n");
    expect_failure(tree.path("main.toro"), "type 'Hidden' is private to module 'helper'");

    tree.write("main.toro",
        "import helper\n"
        "function main() { print(visible()) }\n");
    static_cast<void>(load_and_check(tree.path("main.toro")));
}

void test_unknown_self_and_cycle_diagnostics()
{
    {
        TestTree tree;
        tree.write("main.toro", "import missing.module\nfunction main() {}\n");
        expect_failure(tree.path("main.toro"), "unknown module 'missing.module'");
    }
    {
        TestTree tree;
        tree.write("main.toro", "import main\nfunction main() {}\n");
        expect_failure(tree.path("main.toro"), "cannot import itself");
    }
    {
        TestTree tree;
        tree.write("main.toro", "import one\nfunction main() {}\n");
        tree.write("one.toro", "import two\npublic function one() {}\n");
        tree.write("two.toro", "import one\npublic function two() {}\n");
        expect_failure(tree.path("main.toro"), "import cycle: one -> two -> one");
    }
}

void test_module_qualified_declaration_identity()
{
    TestTree tree;
    tree.write("alpha.toro",
        "public interface Named { function label() -> string }\n"
        "public enum State {\n    ready\n    done\n}\n"
        "public struct Box<T> { value: T }\n"
        "public class Item implements Named {\n"
        "    public value: int\n"
        "    public function label() -> string { return \"alpha\" }\n"
        "}\n"
        "public function helper(value: int) -> int { return value + 1 }\n"
        "function boxed() -> Result<Box<int>, string> {\n"
        "    return ok(Box<int>(value: helper(10)))\n"
        "}\n"
        "public function alpha_value() -> int {\n"
        "    boxes := List<Box<int>>(Box<int>(value: helper(10)))\n"
        "    box := boxes[0]\n"
        "    item := Item(value: box.value)\n"
        "    state := State::ready\n"
        "    handle state {\n"
        "        ready { print(item.label()) }\n"
        "        done {}\n"
        "    }\n"
        "    result := boxed()\n"
        "    handle result {\n"
        "        ok(value) { return value.value }\n"
        "        error(problem) { return 0 }\n"
        "    }\n"
        "}\n");
    tree.write("beta.toro",
        "public interface Named { function label() -> string }\n"
        "public enum State {\n    ready\n    done\n}\n"
        "public struct Box<T> { value: T }\n"
        "public class Item implements Named {\n"
        "    public value: int\n"
        "    public function label() -> string { return \"beta\" }\n"
        "}\n"
        "public function helper(value: int) -> int { return value + 2 }\n"
        "public function beta_value() -> int {\n"
        "    box := Box<int>(value: helper(20))\n"
        "    item := Item(value: box.value)\n"
        "    state := State::done\n"
        "    handle state {\n"
        "        ready {}\n"
        "        done { print(item.label()) }\n"
        "    }\n"
        "    return item.value\n"
        "}\n");
    tree.write("main.toro",
        "import alpha\n"
        "import beta\n"
        "function main() -> int {\n"
        "    if alpha_value() == 11 and beta_value() == 22 { return 0 }\n"
        "    return 1\n"
        "}\n");

    auto program = load_and_check(tree.path("main.toro"));
    const std::string generated = toro::CGenerator().generate(program);
    expect(generated.find("module__5_alpha__3_Box") != std::string::npos,
        "alpha generic nominal identity was not module-qualified");
    expect(generated.find("module__4_beta__3_Box") != std::string::npos,
        "beta generic nominal identity was not module-qualified");
    expect(generated.find("module__5_alpha__helper") != std::string::npos,
        "alpha free function identity was not module-qualified");
    expect(generated.find("module__4_beta__helper") != std::string::npos,
        "beta free function identity was not module-qualified");
    expect(generated.find(tree.path("").string()) == std::string::npos,
        "generated symbols leaked an absolute source-root path");
    auto repeated_program = load_and_check(tree.path("main.toro"));
    expect(generated == toro::CGenerator().generate(repeated_program),
        "module-qualified output changed between identical compilations");
    expect(toro::NativeCompiler().run(generated) == 0,
        "same-named module declarations collided at runtime");
}

void test_cross_module_inheritance_and_interfaces()
{
    TestTree tree;
    tree.write("api.toro",
        "public interface Named { function label() -> string }\n");
    tree.write("model/base.toro",
        "public class Base {\n"
        "    public name: string\n"
        "    public virtual function label() -> string { return self.name }\n"
        "}\n");
    tree.write("model/derived.toro",
        "import api\n"
        "import model.base\n"
        "public class Derived : Base implements Named {\n"
        "    public override function label() -> string { return \"derived\" }\n"
        "}\n"
        "public function exercise() -> string {\n"
        "    value := Derived(name: \"Toro\")\n"
        "    named: Named = value\n"
        "    base: Base = value\n"
        "    return named.label()\n"
        "}\n");
    tree.write("main.toro",
        "import model.derived\n"
        "function main() -> int {\n"
        "    if exercise() == \"derived\" { return 0 }\n"
        "    return 1\n"
        "}\n");

    auto program = load_and_check(tree.path("main.toro"));
    const std::string generated = toro::CGenerator().generate(program);
    expect(toro::NativeCompiler().run(generated) == 0,
        "cross-module inheritance/interface dispatch failed");
}

void test_ambiguous_direct_imports()
{
    TestTree tree;
    tree.write("left.toro",
        "public struct State { value: int }\n"
        "public function choose(value: int) -> int { return value }\n");
    tree.write("right.toro",
        "public struct State { value: int }\n"
        "public function choose(value: int) -> int { return value + 1 }\n");
    tree.write("main.toro",
        "import left\nimport right\n"
        "function consume(value: State) {}\n"
        "function main() {}\n");
    expect_failure(tree.path("main.toro"), "ambiguous imported type 'State'");

    tree.write("main.toro",
        "import left\nimport right\n"
        "function main() { print(choose(1)) }\n");
    expect_failure(tree.path("main.toro"), "ambiguous imported name 'choose'");
}

} // namespace

int main()
{
    try {
        test_import_lexing_and_ast();
        test_loading_visibility_diamond_and_runtime();
        test_private_visibility();
        test_unknown_self_and_cycle_diagnostics();
        test_module_qualified_declaration_identity();
        test_cross_module_inheritance_and_interfaces();
        test_ambiguous_direct_imports();
    } catch (const std::exception& error) {
        std::cerr << "module test failure: " << error.what() << '\n';
        return 1;
    }
    std::cout << "module tests passed\n";
    return 0;
}
