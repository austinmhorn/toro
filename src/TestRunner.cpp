#include "toro/TestRunner.hpp"

#include "toro/AST.hpp"
#include "toro/CGenerator.hpp"
#include "toro/ModuleLoader.hpp"
#include "toro/NativeCompiler.hpp"
#include "toro/SemanticAnalyzer.hpp"
#include "toro/TypeChecker.hpp"

#include <algorithm>
#include <filesystem>
#include <memory>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace toro {
namespace {

struct TestCase {
    std::filesystem::path path;
    std::string relative_path;
    std::string name;
};

bool is_test_name(std::string_view name)
{
    return name.size() > 5 && name.starts_with("test_");
}

bool is_bool_return(const FunctionDeclarationStmt& function)
{
    return function.return_type
        && function.return_type->name == "bool"
        && function.return_type->arguments.empty()
        && !function.return_type->nullable;
}

std::unique_ptr<ReturnStmt> return_integer(std::string value)
{
    return std::make_unique<ReturnStmt>(
        SourceLocation{1, 1}, std::make_unique<IntegerExpr>(std::move(value)));
}

void add_test_main(Program& program, const std::string& test_name)
{
    std::vector<CallArgument> no_arguments;
    auto call = std::make_unique<CallExpr>(
        std::make_unique<IdentifierExpr>(test_name),
        std::vector<TypeReference>{},
        std::move(no_arguments));

    std::vector<std::unique_ptr<Stmt>> then_statements;
    then_statements.push_back(return_integer("0"));
    std::vector<std::unique_ptr<Stmt>> else_statements;
    else_statements.push_back(return_integer("1"));
    auto conditional = std::make_unique<IfStmt>(
        SourceLocation{1, 1},
        std::move(call),
        std::make_unique<BlockStmt>(
            SourceLocation{1, 1}, std::move(then_statements)),
        std::make_unique<BlockStmt>(
            SourceLocation{1, 1}, std::move(else_statements)));

    std::vector<std::unique_ptr<Stmt>> body;
    body.push_back(std::move(conditional));
    auto main = std::make_unique<FunctionDeclarationStmt>(
        SourceLocation{1, 1},
        "main",
        std::vector<GenericParameter>{},
        std::vector<Parameter>{},
        TypeReference{"int", {}, SourceLocation{1, 1}, false, std::nullopt},
        std::make_unique<BlockStmt>(SourceLocation{1, 1}, std::move(body)));
    main->module_name = program.entry_module;
    program.statements.push_back(std::move(main));
    for (auto& module : program.modules) {
        if (module.name == program.entry_module) {
            module.statement_end = program.statements.size();
            break;
        }
    }
}

std::vector<std::filesystem::path> test_files(
    const std::filesystem::path& root)
{
    std::vector<std::filesystem::path> result;
    if (!std::filesystem::is_directory(root)) {
        return result;
    }
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (entry.is_regular_file() && entry.path().extension() == ".toro") {
            result.push_back(entry.path());
        }
    }
    std::ranges::sort(result, [&](const auto& left, const auto& right) {
        return std::filesystem::relative(left, root).generic_string()
            < std::filesystem::relative(right, root).generic_string();
    });
    return result;
}

} // namespace

int TestRunner::run(
    const ProjectConfig& project,
    std::ostream& output) const
{
    const auto root = project.source_root / "tests";
    std::vector<TestCase> tests;
    std::size_t failed = 0;
    std::size_t passed = 0;
    for (const auto& path : test_files(root)) {
        const std::string relative =
            std::filesystem::relative(path, project.source_root).generic_string();
        try {
            const Program program = ModuleLoader().load(path, project.source_root);
            const auto module = std::ranges::find_if(
                program.modules,
                [&](const Program::ModuleInfo& candidate) {
                    return candidate.name == program.entry_module;
                });
            if (module == program.modules.end()) {
                throw std::runtime_error("test entry module metadata is missing");
            }
            for (std::size_t index = module->statement_begin;
                 index < module->statement_end; ++index) {
                const Stmt& statement = *program.statements[index];
                if (statement.kind != StmtKind::FunctionDeclaration) continue;
                const auto& function =
                    static_cast<const FunctionDeclarationStmt&>(statement);
                if (!is_test_name(function.name)) continue;
                if (!function.generic_parameters.empty()
                    || !function.parameters.empty()
                    || !is_bool_return(function)) {
                    output << "FAIL " << relative << "::" << function.name
                        << "\n  test functions must be non-generic, take no parameters, and return bool\n";
                    ++failed;
                    continue;
                }
                tests.push_back(TestCase{path, relative, function.name});
            }
        } catch (const std::exception& error) {
            output << "FAIL " << relative << "\n  " << error.what() << '\n';
            ++failed;
        }
    }

    std::ranges::sort(tests, [](const TestCase& left, const TestCase& right) {
        return std::pair{left.relative_path, left.name}
            < std::pair{right.relative_path, right.name};
    });

    for (const auto& test : tests) {
        try {
            Program program = ModuleLoader().load(test.path, project.source_root);
            add_test_main(program, test.name);
            SemanticAnalyzer().analyze(program);
            TypeChecker().check(program);
            const int status = NativeCompiler().run(CGenerator().generate(program));
            if (status == 0) {
                output << "PASS " << test.relative_path << "::" << test.name << '\n';
                ++passed;
            } else {
                output << "FAIL " << test.relative_path << "::" << test.name
                    << "\n  exited with status " << status << '\n';
                ++failed;
            }
        } catch (const std::exception& error) {
            output << "FAIL " << test.relative_path << "::" << test.name
                << "\n  " << error.what() << '\n';
            ++failed;
        }
    }

    if (tests.empty() && failed == 0) {
        output << "no tests found under '" << root.string() << "'\n";
        return 1;
    }
    output << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}

} // namespace toro
