#include "toro/ArcOptimizer.hpp"
#include "toro/BorrowInferenceAnalyzer.hpp"
#include "toro/CGenerator.hpp"
#include "toro/Diagnostic.hpp"
#include "toro/EffectAnalyzer.hpp"
#include "toro/EscapeAnalyzer.hpp"
#include "toro/Formatter.hpp"
#include "toro/IRLowerer.hpp"
#include "toro/Lexer.hpp"
#include "toro/ModuleLoader.hpp"
#include "toro/NativeCompiler.hpp"
#include "toro/OwnershipSummaryAnalyzer.hpp"
#include "toro/PerformanceContractAnalyzer.hpp"
#include "toro/Parser.hpp"
#include "toro/ProjectConfig.hpp"
#include "toro/SemanticAnalyzer.hpp"
#include "toro/StorageDecisionAnalyzer.hpp"
#include "toro/StrongCycleAnalyzer.hpp"
#include "toro/SourceFile.hpp"
#include "toro/Token.hpp"
#include "toro/TypeChecker.hpp"
#include "toro/TestRunner.hpp"

#include <algorithm>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

struct ReportedDiagnostics {
};

void print_usage(std::ostream& output)
{
    output << "Usage: toro <source.toro>\n"
              "       toro tokens <source.toro>\n"
              "       toro ast-expression <source.toro>\n"
              "       toro ast <source.toro>\n"
              "       toro ir [--effects | --ownership | --escape | --arc [--no-opt]] [source.toro]\n"
              "       toro check [source.toro]\n"
              "       toro emit-c [source.toro] [-o <output.c>]\n"
              "       toro build [source.toro] [-o <output>]\n"
              "       toro run [source.toro]\n"
              "       toro fmt [--check] [source.toro]\n"
              "       toro test\n"
              "       toro project-info --cmake\n"
              "       toro --version\n";
}

bool is_project_command(std::string_view command)
{
    return command == "check" || command == "ir" || command == "emit-c"
        || command == "build" || command == "run";
}

struct CompilationInput {
    std::filesystem::path entry_path;
    std::optional<std::filesystem::path> source_root;
    std::optional<std::string> project_name;
    std::vector<std::string> c_libraries;
};

CompilationInput direct_input(const std::filesystem::path& path)
{
    return CompilationInput{path, std::nullopt, std::nullopt, {}};
}

CompilationInput project_input()
{
    const auto project = toro::ProjectConfigLoader().discover();
    return CompilationInput{
        project.entry_path,
        project.source_root,
        project.name,
        project.c_libraries,
    };
}

toro::Program load_program(const CompilationInput& input)
{
    if (input.source_root) {
        return toro::ModuleLoader().load(input.entry_path, *input.source_root);
    }
    return toro::ModuleLoader().load(input.entry_path);
}

bool is_source_command(std::string_view command)
{
    return command == "tokens" || command == "ast-expression"
        || command == "ast" || command == "check" || command == "emit-c"
        || command == "ir" || command == "build" || command == "run";
}

void print_tokens(std::string_view source)
{
    const auto tokens = toro::Lexer(source).tokenize();
    for (const auto& token : tokens) {
        std::cout << toro::token_type_name(token.type);
        if (!token.lexeme.empty()) {
            std::cout << '\t' << token.lexeme;
        }
        std::cout << '\n';
    }
}

void print_expression_ast(std::string_view source)
{
    auto tokens = toro::Lexer(source).tokenize();
    auto expression = toro::Parser(std::move(tokens)).parse_expression();
    std::cout << toro::dump_expression(*expression);
}

void print_ast(std::string_view source)
{
    auto tokens = toro::Lexer(source).tokenize();
    const auto program = toro::Parser(std::move(tokens)).parse_program();
    std::cout << toro::dump_program(program);
}

void analyze_runtime_plan(toro::IRProgram& ir)
{
    toro::EffectAnalyzer().analyze(ir);
    toro::OwnershipSummaryAnalyzer().analyze(ir);
    toro::StrongCycleAnalyzer().analyze(ir);
    toro::EscapeAnalyzer().analyze(ir);
    toro::StorageDecisionAnalyzer().analyze(ir);
    toro::ArcMaterializer().materialize(ir);
    toro::OwnershipTransferAnalyzer().analyze(ir);
    toro::BorrowInferenceAnalyzer().analyze(ir);
    toro::ArcOptimizer().optimize(ir);
    toro::PerformanceContractAnalyzer().analyze(ir);
}

void print_diagnostics(const toro::IRProgram& ir)
{
    for (const auto& diagnostic : ir.diagnostics) {
        std::cerr << toro::format_diagnostic(diagnostic);
    }
}

void enforce_diagnostics(const toro::IRProgram& ir)
{
    print_diagnostics(ir);
    if (toro::has_error_diagnostics(ir.diagnostics)) {
        throw ReportedDiagnostics{};
    }
}

void check_source(const CompilationInput& input)
{
    const auto program = load_program(input);
    toro::SemanticAnalyzer().analyze(program);
    toro::TypeChecker().check(program);
    auto ir = toro::IRLowerer().lower(program);
    analyze_runtime_plan(ir);
    enforce_diagnostics(ir);
    std::cout << "check passed\n";
}

std::string generate_c(const CompilationInput& input)
{
    auto program = load_program(input);
    toro::SemanticAnalyzer().analyze(program);
    toro::TypeChecker().check(program);
    auto ir = toro::IRLowerer().lower(program);
    analyze_runtime_plan(ir);
    enforce_diagnostics(ir);
    return toro::CGenerator().generate(ir);
}

void print_ir(
    const CompilationInput& input,
    bool include_effects = false,
    bool include_arc = false,
    bool optimize_arc = true,
    bool include_escape = false,
    bool include_ownership = false)
{
    auto program = load_program(input);
    toro::SemanticAnalyzer().analyze(program);
    toro::TypeChecker().check(program);
    auto ir = toro::IRLowerer().lower(program);
    analyze_runtime_plan(ir);
    enforce_diagnostics(ir);
    if (include_arc && !optimize_arc) {
        toro::ArcMaterializer().materialize(ir);
    }
    std::cout << toro::dump_ir(
        ir, include_effects, include_arc, include_escape, include_ownership);
}

void emit_c(
    const CompilationInput& input,
    const std::optional<std::filesystem::path>& output_path = std::nullopt)
{
    const std::string generated = generate_c(input);
    if (!output_path) {
        std::cout << generated;
        return;
    }
    std::ofstream output(*output_path, std::ios::binary);
    if (!output) {
        throw std::runtime_error(
            "could not write generated C file '" + output_path->string() + "'");
    }
    output.write(generated.data(), static_cast<std::streamsize>(generated.size()));
    if (!output) {
        throw std::runtime_error(
            "failed while writing generated C file '" + output_path->string() + "'");
    }
}

std::string cmake_bracket_argument(std::string_view value)
{
    std::string equals;
    while (value.find("]" + equals + "]") != std::string_view::npos) {
        equals += '=';
    }
    return "[" + equals + "[" + std::string(value) + "]" + equals + "]";
}

void print_cmake_project_info()
{
    const auto project = toro::ProjectConfigLoader().discover();
    std::cout << "set(TORO_PROJECT_INFO_NAME "
              << cmake_bracket_argument(project.name) << ")\n"
              << "set(TORO_PROJECT_INFO_SOURCE_ROOT "
              << cmake_bracket_argument(project.source_root.string()) << ")\n"
              << "set(TORO_PROJECT_INFO_ENTRY "
              << cmake_bracket_argument(project.entry_path.string()) << ")\n"
              << "set(TORO_PROJECT_INFO_C_LIBRARIES";
    for (const auto& library : project.c_libraries) {
        std::cout << ' ' << cmake_bracket_argument(library);
    }
    std::cout << ")\n";
}

void build_native(
    const CompilationInput& input,
    const std::filesystem::path& output_path)
{
    toro::NativeCompiler().build(
        generate_c(input), output_path, input.c_libraries);
    std::cout << "built " << std::filesystem::absolute(output_path).string() << '\n';
}

int run_native(const CompilationInput& input)
{
    return toro::NativeCompiler().run(generate_c(input), input.c_libraries);
}

std::vector<std::filesystem::path> project_source_files(
    const std::filesystem::path& source_root)
{
    std::vector<std::filesystem::path> paths;
    for (const auto& entry :
         std::filesystem::recursive_directory_iterator(source_root)) {
        if (entry.is_regular_file() && entry.path().extension() == ".toro") {
            paths.push_back(entry.path());
        }
    }
    std::ranges::sort(paths, [&](const auto& left, const auto& right) {
        return std::filesystem::relative(left, source_root).generic_string()
            < std::filesystem::relative(right, source_root).generic_string();
    });
    return paths;
}

int format_sources(int argc, char* argv[])
{
    bool check_only = false;
    std::optional<std::filesystem::path> direct_path;
    if (argc == 3) {
        if (std::string_view(argv[2]) == "--check") {
            check_only = true;
        } else {
            direct_path = argv[2];
        }
    } else if (argc == 4 && std::string_view(argv[2]) == "--check") {
        check_only = true;
        direct_path = argv[3];
    } else if (argc != 2) {
        throw std::invalid_argument(
            "usage: toro fmt [--check] [source.toro]");
    }

    std::vector<std::filesystem::path> paths;
    std::optional<std::filesystem::path> display_root;
    if (direct_path) {
        paths.push_back(*direct_path);
    } else {
        const auto project = toro::ProjectConfigLoader().discover();
        display_root = project.source_root;
        paths = project_source_files(project.source_root);
    }
    const auto changed = toro::Formatter().format_files(paths, check_only);
    if (check_only) {
        if (changed.empty()) {
            std::cout << "format check passed\n";
            return 0;
        }
        for (const auto& path : changed) {
            std::cout << "would reformat ";
            if (display_root) {
                std::cout << std::filesystem::relative(path, *display_root)
                                 .generic_string();
            } else {
                std::cout << path.string();
            }
            std::cout << '\n';
        }
        return 1;
    }
    if (changed.empty()) {
        std::cout << "already formatted\n";
    } else {
        for (const auto& path : changed) {
            std::cout << "formatted ";
            if (display_root) {
                std::cout << std::filesystem::relative(path, *display_root)
                                 .generic_string();
            } else {
                std::cout << path.string();
            }
            std::cout << '\n';
        }
    }
    return 0;
}

} // namespace

int main(int argc, char* argv[])
{
    if (argc == 2 && std::string_view(argv[1]) == "--version") {
        std::cout << "toro compiler v0.1.0\n";
        return 0;
    }

    if (argc >= 2 && std::string_view(argv[1]) == "fmt") {
        try {
            return format_sources(argc, argv);
        } catch (const std::exception& error) {
            std::cerr << "error: " << error.what() << '\n';
            return 1;
        }
    }
    if (argc == 2 && std::string_view(argv[1]) == "test") {
        try {
            const auto project = toro::ProjectConfigLoader().discover();
            return toro::TestRunner().run(project, std::cout);
        } catch (const std::exception& error) {
            std::cerr << "error: " << error.what() << '\n';
            return 1;
        }
    }
    if (argc == 3 && std::string_view(argv[1]) == "project-info"
        && std::string_view(argv[2]) == "--cmake") {
        try {
            print_cmake_project_info();
            return 0;
        } catch (const std::exception& error) {
            std::cerr << "error: " << error.what() << '\n';
            return 1;
        }
    }

    if (argc == 2 && is_source_command(argv[1])
        && !is_project_command(argv[1])) {
        std::cerr << "error: " << argv[1] << " requires a source file\n";
        print_usage(std::cerr);
        return 1;
    }

    const bool standard_source_command = argc == 3 && is_source_command(argv[1]);
    const bool project_command = argc == 2 && is_project_command(argv[1]);
    const bool project_ir_effects = argc == 3
        && std::string_view(argv[1]) == "ir"
        && std::string_view(argv[2]) == "--effects";
    const bool file_ir_effects = argc == 4
        && std::string_view(argv[1]) == "ir"
        && std::string_view(argv[2]) == "--effects";
    const bool project_ir_ownership = argc == 3
        && std::string_view(argv[1]) == "ir"
        && std::string_view(argv[2]) == "--ownership";
    const bool file_ir_ownership = argc == 4
        && std::string_view(argv[1]) == "ir"
        && std::string_view(argv[2]) == "--ownership";
    const bool project_ir_escape = argc == 3
        && std::string_view(argv[1]) == "ir"
        && std::string_view(argv[2]) == "--escape";
    const bool file_ir_escape = argc == 4
        && std::string_view(argv[1]) == "ir"
        && std::string_view(argv[2]) == "--escape";
    const bool project_ir_arc = argc == 3
        && std::string_view(argv[1]) == "ir"
        && std::string_view(argv[2]) == "--arc";
    const bool file_ir_arc = argc == 4
        && std::string_view(argv[1]) == "ir"
        && std::string_view(argv[2]) == "--arc";
    const bool project_ir_arc_unoptimized = argc == 4
        && std::string_view(argv[1]) == "ir"
        && std::string_view(argv[2]) == "--arc"
        && std::string_view(argv[3]) == "--no-opt";
    const bool file_ir_arc_unoptimized = argc == 5
        && std::string_view(argv[1]) == "ir"
        && std::string_view(argv[2]) == "--arc"
        && std::string_view(argv[3]) == "--no-opt";
    const bool project_build_with_output = argc == 4
        && std::string_view(argv[1]) == "build"
        && std::string_view(argv[2]) == "-o";
    const bool project_emit_c_with_output = argc == 4
        && std::string_view(argv[1]) == "emit-c"
        && std::string_view(argv[2]) == "-o";
    const bool file_build_with_output = argc == 5
        && std::string_view(argv[1]) == "build"
        && std::string_view(argv[3]) == "-o";
    const bool file_emit_c_with_output = argc == 5
        && std::string_view(argv[1]) == "emit-c"
        && std::string_view(argv[3]) == "-o";
    if (argc != 2 && !standard_source_command && !project_ir_effects
        && !file_ir_effects && !project_ir_ownership && !file_ir_ownership
        && !project_ir_arc && !file_ir_arc
        && !project_ir_escape && !file_ir_escape
        && !project_ir_arc_unoptimized && !file_ir_arc_unoptimized
        && !project_build_with_output
        && !project_emit_c_with_output && !file_build_with_output
        && !file_emit_c_with_output) {
        std::cerr << "error: invalid arguments\n";
        print_usage(std::cerr);
        return 1;
    }

    try {
        if (file_ir_arc_unoptimized) {
            print_ir(direct_input(argv[4]), false, true, false);
        } else if (project_ir_arc_unoptimized) {
            print_ir(project_input(), false, true, false);
        } else if (file_ir_arc) {
            print_ir(direct_input(argv[3]), false, true);
        } else if (project_ir_arc) {
            print_ir(project_input(), false, true);
        } else if (file_ir_effects) {
            print_ir(direct_input(argv[3]), true);
        } else if (project_ir_effects) {
            print_ir(project_input(), true);
        } else if (file_ir_ownership) {
            print_ir(direct_input(argv[3]), false, false, true, false, true);
        } else if (project_ir_ownership) {
            print_ir(project_input(), false, false, true, false, true);
        } else if (file_ir_escape) {
            print_ir(direct_input(argv[3]), false, false, true, true);
        } else if (project_ir_escape) {
            print_ir(project_input(), false, false, true, true);
        } else if (file_emit_c_with_output) {
            emit_c(direct_input(argv[2]), argv[4]);
        } else if (project_emit_c_with_output) {
            emit_c(project_input(), argv[3]);
        } else if (file_build_with_output) {
            build_native(direct_input(argv[2]), argv[4]);
        } else if (project_build_with_output) {
            build_native(project_input(), argv[3]);
        } else if (project_command) {
            const auto input = project_input();
            const std::string_view command(argv[1]);
            if (command == "check") {
                check_source(input);
            } else if (command == "ir") {
                print_ir(input);
            } else if (command == "emit-c") {
                emit_c(input);
            } else if (command == "build") {
                build_native(input,
                    std::filesystem::current_path() / *input.project_name);
            } else {
                return run_native(input);
            }
        } else if (argc == 3) {
            if (std::string_view(argv[1]) == "tokens") {
                print_tokens(toro::load_source_file(argv[2]).contents);
            } else if (std::string_view(argv[1]) == "ast-expression") {
                print_expression_ast(toro::load_source_file(argv[2]).contents);
            } else if (std::string_view(argv[1]) == "check") {
                check_source(direct_input(argv[2]));
            } else if (std::string_view(argv[1]) == "ir") {
                print_ir(direct_input(argv[2]));
            } else if (std::string_view(argv[1]) == "emit-c") {
                emit_c(direct_input(argv[2]));
            } else if (std::string_view(argv[1]) == "build") {
                const auto output = std::filesystem::current_path()
                    / std::filesystem::path(argv[2]).stem();
                build_native(direct_input(argv[2]), output);
            } else if (std::string_view(argv[1]) == "run") {
                return run_native(direct_input(argv[2]));
            } else {
                print_ast(toro::load_source_file(argv[2]).contents);
            }
        } else {
            const auto source = toro::load_source_file(argv[1]);
            std::cout << source.contents;
        }
    } catch (const ReportedDiagnostics&) {
        return 1;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }

    return 0;
}
