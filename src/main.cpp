#include "toro/CGenerator.hpp"
#include "toro/Lexer.hpp"
#include "toro/ModuleLoader.hpp"
#include "toro/NativeCompiler.hpp"
#include "toro/Parser.hpp"
#include "toro/ProjectConfig.hpp"
#include "toro/SemanticAnalyzer.hpp"
#include "toro/SourceFile.hpp"
#include "toro/Token.hpp"
#include "toro/TypeChecker.hpp"

#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace {

void print_usage(std::ostream& output)
{
    output << "Usage: toro <source.toro>\n"
              "       toro tokens <source.toro>\n"
              "       toro ast-expression <source.toro>\n"
              "       toro ast <source.toro>\n"
              "       toro check [source.toro]\n"
              "       toro emit-c [source.toro]\n"
              "       toro build [source.toro] [-o <output>]\n"
              "       toro run [source.toro]\n"
              "       toro --version\n";
}

bool is_project_command(std::string_view command)
{
    return command == "check" || command == "emit-c"
        || command == "build" || command == "run";
}

struct CompilationInput {
    std::filesystem::path entry_path;
    std::optional<std::filesystem::path> source_root;
    std::optional<std::string> project_name;
};

CompilationInput direct_input(const std::filesystem::path& path)
{
    return CompilationInput{path, std::nullopt, std::nullopt};
}

CompilationInput project_input()
{
    const auto project = toro::ProjectConfigLoader().discover();
    return CompilationInput{
        project.entry_path,
        project.source_root,
        project.name,
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
        || command == "build" || command == "run";
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

void check_source(const CompilationInput& input)
{
    const auto program = load_program(input);
    toro::SemanticAnalyzer().analyze(program);
    toro::TypeChecker().check(program);
    std::cout << "check passed\n";
}

std::string generate_c(const CompilationInput& input)
{
    const auto program = load_program(input);
    toro::SemanticAnalyzer().analyze(program);
    toro::TypeChecker().check(program);
    return toro::CGenerator().generate(program);
}

void emit_c(const CompilationInput& input)
{
    std::cout << generate_c(input);
}

void build_native(
    const CompilationInput& input,
    const std::filesystem::path& output_path)
{
    toro::NativeCompiler().build(generate_c(input), output_path);
    std::cout << "built " << std::filesystem::absolute(output_path).string() << '\n';
}

int run_native(const CompilationInput& input)
{
    return toro::NativeCompiler().run(generate_c(input));
}

} // namespace

int main(int argc, char* argv[])
{
    if (argc == 2 && std::string_view(argv[1]) == "--version") {
        std::cout << "toro compiler v0.1.0\n";
        return 0;
    }

    if (argc == 2 && is_source_command(argv[1])
        && !is_project_command(argv[1])) {
        std::cerr << "error: " << argv[1] << " requires a source file\n";
        print_usage(std::cerr);
        return 1;
    }

    const bool standard_source_command = argc == 3 && is_source_command(argv[1]);
    const bool project_command = argc == 2 && is_project_command(argv[1]);
    const bool project_build_with_output = argc == 4
        && std::string_view(argv[1]) == "build"
        && std::string_view(argv[2]) == "-o";
    const bool file_build_with_output = argc == 5
        && std::string_view(argv[1]) == "build"
        && std::string_view(argv[3]) == "-o";
    if (argc != 2 && !standard_source_command && !project_build_with_output
        && !file_build_with_output) {
        std::cerr << "error: invalid arguments\n";
        print_usage(std::cerr);
        return 1;
    }

    try {
        if (file_build_with_output) {
            build_native(direct_input(argv[2]), argv[4]);
        } else if (project_build_with_output) {
            build_native(project_input(), argv[3]);
        } else if (project_command) {
            const auto input = project_input();
            const std::string_view command(argv[1]);
            if (command == "check") {
                check_source(input);
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
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }

    return 0;
}
