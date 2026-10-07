#include "toro/ModuleLoader.hpp"

#include "toro/Lexer.hpp"
#include "toro/Parser.hpp"
#include "toro/SourceFile.hpp"

#include <algorithm>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace toro {
namespace {

struct ParsedModule {
    std::string name;
    std::filesystem::path path;
    Program program;
    std::vector<std::string> imports;
};

[[noreturn]] void throw_module_error(
    const ParsedModule& module,
    SourceLocation location,
    const std::string& message)
{
    throw std::runtime_error(
        module.path.string() + ":" + std::to_string(location.line) + ":"
        + std::to_string(location.column) + ": module error: " + message);
}

std::filesystem::path module_path(
    const std::filesystem::path& source_root,
    const std::string& module_name)
{
    std::filesystem::path result = source_root;
    std::size_t start = 0;
    while (start < module_name.size()) {
        const std::size_t separator = module_name.find('.', start);
        result /= module_name.substr(
            start,
            separator == std::string::npos
                ? std::string::npos
                : separator - start);
        if (separator == std::string::npos) {
            break;
        }
        start = separator + 1;
    }
    result += ".toro";
    return result;
}

class LoadSession {
public:
    explicit LoadSession(std::filesystem::path source_root)
        : source_root_(std::move(source_root))
    {
    }

    Program load(
        const std::filesystem::path& entry_path,
        const std::string& entry_name)
    {
        load_module(entry_name, entry_path, std::nullopt);
        visit(entry_name);

        Program result;
        result.entry_module = entry_name;
        for (const auto& name : emission_order_) {
            auto& module = modules_.at(name);
            result.modules.push_back(Program::ModuleInfo{
                module.name,
                module.path.string(),
                module.imports,
            });
            for (auto& statement : module.program.statements) {
                if (statement->kind == StmtKind::ImportDeclaration) {
                    continue;
                }
                if (module.name != entry_name
                    && statement->kind == StmtKind::FunctionDeclaration
                    && static_cast<const FunctionDeclarationStmt&>(*statement).name
                        == "main") {
                    throw_module_error(
                        module,
                        statement->location,
                        "only the entry module may declare 'main'");
                }
                statement->module_name = module.name;
                result.statements.push_back(std::move(statement));
            }
        }
        return result;
    }

private:
    void load_module(
        const std::string& name,
        const std::filesystem::path& path,
        std::optional<std::pair<const ParsedModule*, SourceLocation>> importer)
    {
        if (modules_.contains(name)) {
            return;
        }
        if (!std::filesystem::exists(path)) {
            if (importer) {
                throw_module_error(
                    *importer->first,
                    importer->second,
                    "unknown module '" + name + "'");
            }
            throw std::runtime_error("source file does not exist: " + path.string());
        }

        const SourceFile source = load_source_file(path);
        Program parsed;
        try {
            parsed = Parser(Lexer(source.contents).tokenize()).parse_program();
        } catch (const std::exception& error) {
            throw std::runtime_error(
                "in module '" + name + "' (" + path.string() + "): "
                + error.what());
        }

        ParsedModule module{name, std::filesystem::absolute(path), std::move(parsed), {}};
        std::unordered_set<std::string> seen_imports;
        for (const auto& statement : module.program.statements) {
            if (statement->kind != StmtKind::ImportDeclaration) {
                continue;
            }
            const auto& import = static_cast<const ImportDeclarationStmt&>(*statement);
            const std::string imported_name = import.module_path();
            if (imported_name == name) {
                throw_module_error(
                    module,
                    import.location,
                    "module '" + name + "' cannot import itself");
            }
            if (seen_imports.insert(imported_name).second) {
                module.imports.push_back(imported_name);
            }
        }

        const auto [position, inserted] = modules_.emplace(name, std::move(module));
        static_cast<void>(inserted);
        for (const auto& imported_name : position->second.imports) {
            const auto import_statement = std::ranges::find_if(
                position->second.program.statements,
                [&](const std::unique_ptr<Stmt>& statement) {
                    return statement->kind == StmtKind::ImportDeclaration
                        && static_cast<const ImportDeclarationStmt&>(*statement)
                                .module_path()
                            == imported_name;
                });
            load_module(
                imported_name,
                module_path(source_root_, imported_name),
                std::pair{&position->second, (*import_statement)->location});
        }
    }

    void visit(const std::string& name)
    {
        const int state = states_[name];
        if (state == 2) {
            return;
        }
        if (state == 1) {
            const auto start = std::ranges::find(stack_, name);
            std::string cycle;
            for (auto current = start; current != stack_.end(); ++current) {
                if (!cycle.empty()) {
                    cycle += " -> ";
                }
                cycle += *current;
            }
            cycle += " -> " + name;
            const auto& module = modules_.at(stack_.back());
            throw_module_error(
                module, SourceLocation{1, 1}, "import cycle: " + cycle);
        }
        states_[name] = 1;
        stack_.push_back(name);
        for (const auto& imported : modules_.at(name).imports) {
            visit(imported);
        }
        stack_.pop_back();
        states_[name] = 2;
        emission_order_.push_back(name);
    }

    std::filesystem::path source_root_;
    std::map<std::string, ParsedModule> modules_;
    std::map<std::string, int> states_;
    std::vector<std::string> stack_;
    std::vector<std::string> emission_order_;
};

} // namespace

Program ModuleLoader::load(const std::filesystem::path& entry_path) const
{
    if (entry_path.extension() != ".toro") {
        throw std::invalid_argument("toro source files must use the .toro extension");
    }
    const std::filesystem::path absolute_entry = std::filesystem::absolute(entry_path);
    const std::filesystem::path source_root = absolute_entry.parent_path();
    const std::string entry_name = absolute_entry.stem().string();
    return LoadSession(source_root).load(absolute_entry, entry_name);
}

} // namespace toro
