#include "toro/CGenerator.hpp"
#include "toro/ModuleLoader.hpp"
#include "toro/NativeCompiler.hpp"
#include "toro/ProjectConfig.hpp"
#include "toro/SemanticAnalyzer.hpp"
#include "toro/TypeChecker.hpp"

#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

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

class ProjectTree {
public:
    ProjectTree()
    {
#if defined(_WIN32)
        root_ = std::filesystem::temp_directory_path() / "toro-project-tests";
#else
        root_ = std::filesystem::temp_directory_path()
            / ("toro-project-tests-" + std::to_string(::getpid()));
#endif
        std::filesystem::remove_all(root_);
        std::filesystem::create_directories(root_);
    }

    ~ProjectTree()
    {
        std::filesystem::remove_all(root_);
    }

    void write(std::string_view relative, std::string_view contents) const
    {
        const auto path = root_ / relative;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream output(path);
        if (!output) {
            throw std::runtime_error("could not create project test file");
        }
        output << contents;
    }

    [[nodiscard]] const std::filesystem::path& root() const { return root_; }

private:
    std::filesystem::path root_;
};

void expect_error(const std::filesystem::path& manifest, std::string_view expected)
{
    try {
        static_cast<void>(toro::ProjectConfigLoader().load(manifest));
    } catch (const std::exception& error) {
        expect(std::string_view(error.what()).find(expected) != std::string_view::npos,
            std::string("expected error containing '") + std::string(expected)
                + "', got '" + error.what() + "'");
        return;
    }
    throw std::runtime_error("expected project manifest failure");
}

void test_valid_project_pipeline()
{
    ProjectTree tree;
    tree.write("toro.toml",
        "[project]\nname = \"sample-app\"\n"
        "[source]\nroot = \"source\"\nentry = \"app/main.toro\"\n"
        "[ffi]\nlibraries = [\"m\"]\n");
    tree.write("source/game/player.toro",
        "public struct Player { name: string }\n");
    tree.write("source/app/main.toro",
        "import game.player\n"
        "function main() -> int {\n"
        "    player := Player(name: \"Toro\")\n"
        "    if player.name == \"Toro\" { return 0 }\n"
        "    return 1\n"
        "}\n");

    const auto project = toro::ProjectConfigLoader().discover(tree.root());
    expect(project.name == "sample-app", "project name was not parsed");
    expect(project.source_root == tree.root() / "source",
        "configured source root was not retained");
    expect(project.entry_path == tree.root() / "source/app/main.toro",
        "configured entry was not retained");
    expect(project.c_libraries == std::vector<std::string>({"m"}),
        "configured C libraries were not retained in order");

    auto program = toro::ModuleLoader().load(
        project.entry_path, project.source_root);
    expect(program.entry_module == "app.main",
        "nested project entry did not receive a source-root module identity");
    toro::SemanticAnalyzer().analyze(program);
    toro::TypeChecker().check(program);
    const std::string first = toro::CGenerator().generate(program);

    auto repeated = toro::ModuleLoader().load(
        project.entry_path, project.source_root);
    toro::SemanticAnalyzer().analyze(repeated);
    toro::TypeChecker().check(repeated);
    const std::string second = toro::CGenerator().generate(repeated);
    expect(first == second, "project C generation was not deterministic");
    expect(first.find(tree.root().string()) == std::string::npos,
        "generated project C contained an absolute project path");
    expect(toro::NativeCompiler().run(first, project.c_libraries) == 0,
        "configured project did not run successfully");
}

void test_manifest_diagnostics()
{
    {
        ProjectTree tree;
        expect_error(tree.root() / "toro.toml", "project manifest not found");
    }
    {
        ProjectTree tree;
        tree.write("toro.toml", "name = \"bad\"\n");
        expect_error(tree.root() / "toro.toml", "inside a section");
    }
    {
        ProjectTree tree;
        tree.write("toro.toml",
            "[project]\nname = \"bad name\"\n"
            "[source]\nroot = \"src\"\nentry = \"main.toro\"\n");
        tree.write("src/main.toro", "function main() {}\n");
        expect_error(tree.root() / "toro.toml", "project.name");
    }
    {
        ProjectTree tree;
        tree.write("toro.toml",
            "[project]\nname = \"missing-root\"\n"
            "[source]\nroot = \"src\"\nentry = \"main.toro\"\n");
        expect_error(tree.root() / "toro.toml", "source root does not exist");
    }
    {
        ProjectTree tree;
        tree.write("toro.toml",
            "[project]\nname = \"missing-entry\"\n"
            "[source]\nroot = \"src\"\nentry = \"main.toro\"\n");
        std::filesystem::create_directories(tree.root() / "src");
        expect_error(tree.root() / "toro.toml", "entry source does not exist");
    }
    {
        ProjectTree tree;
        tree.write("toro.toml",
            "[project]\nname = \"outside\"\n"
            "[source]\nroot = \"src\"\nentry = \"../main.toro\"\n");
        expect_error(tree.root() / "toro.toml",
            "must remain inside the configured source root");
    }
    {
        ProjectTree tree;
        tree.write("toro.toml",
            "[project]\nname = \"duplicate\"\nname = \"again\"\n"
            "[source]\nroot = \"src\"\nentry = \"main.toro\"\n");
        expect_error(tree.root() / "toro.toml", "duplicate configuration key");
    }
    {
        ProjectTree tree;
        tree.write("toro.toml",
            "[project]\nname = \"bad-ffi\"\n"
            "[source]\nroot = \"src\"\nentry = \"main.toro\"\n"
            "[ffi]\nlibraries = [\"-Wl,unsafe\"]\n");
        tree.write("src/main.toro", "function main() {}\n");
        expect_error(tree.root() / "toro.toml", "invalid library name");
    }
    {
        ProjectTree tree;
        tree.write("toro.toml",
            "[project]\nname = \"bad-ffi\"\n"
            "[source]\nroot = \"src\"\nentry = \"main.toro\"\n"
            "[ffi]\nlibraries = [\"m\", \"m\"]\n");
        tree.write("src/main.toro", "function main() {}\n");
        expect_error(tree.root() / "toro.toml", "duplicate library");
    }
}

} // namespace

int main()
{
    try {
        test_valid_project_pipeline();
        test_manifest_diagnostics();
    } catch (const std::exception& error) {
        std::cerr << "project config test failure: " << error.what() << '\n';
        return 1;
    }
    std::cout << "project config tests passed\n";
    return 0;
}
