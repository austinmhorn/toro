#include "toro/CGenerator.hpp"
#include "toro/Lexer.hpp"
#include "toro/NativeCompiler.hpp"
#include "toro/Parser.hpp"
#include "toro/SemanticAnalyzer.hpp"
#include "toro/SourceFile.hpp"
#include "toro/Token.hpp"
#include "toro/TypeChecker.hpp"

#include <exception>
#include <filesystem>
#include <iostream>
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
              "       toro check <source.toro>\n"
              "       toro emit-c <source.toro>\n"
              "       toro build <source.toro> [-o <output>]\n"
              "       toro run <source.toro>\n"
              "       toro --version\n";
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

void check_source(std::string_view source)
{
    auto tokens = toro::Lexer(source).tokenize();
    const auto program = toro::Parser(std::move(tokens)).parse_program();
    toro::SemanticAnalyzer().analyze(program);
    toro::TypeChecker().check(program);
    std::cout << "check passed\n";
}

std::string generate_c(std::string_view source)
{
    auto tokens = toro::Lexer(source).tokenize();
    const auto program = toro::Parser(std::move(tokens)).parse_program();
    toro::SemanticAnalyzer().analyze(program);
    toro::TypeChecker().check(program);
    return toro::CGenerator().generate(program);
}

void emit_c(std::string_view source)
{
    std::cout << generate_c(source);
}

void build_native(
    std::string_view source,
    const std::filesystem::path& output_path)
{
    toro::NativeCompiler().build(generate_c(source), output_path);
    std::cout << "built " << std::filesystem::absolute(output_path).string() << '\n';
}

int run_native(std::string_view source)
{
    return toro::NativeCompiler().run(generate_c(source));
}

} // namespace

int main(int argc, char* argv[])
{
    if (argc == 2 && std::string_view(argv[1]) == "--version") {
        std::cout << "toro compiler v0.1.0\n";
        return 0;
    }

    if (argc == 2 && is_source_command(argv[1])) {
        std::cerr << "error: " << argv[1] << " requires a source file\n";
        print_usage(std::cerr);
        return 1;
    }

    const bool standard_source_command = argc == 3 && is_source_command(argv[1]);
    const bool build_with_output = argc == 5
        && std::string_view(argv[1]) == "build"
        && std::string_view(argv[3]) == "-o";
    if (argc != 2 && !standard_source_command && !build_with_output) {
        std::cerr << "error: invalid arguments\n";
        print_usage(std::cerr);
        return 1;
    }

    try {
        if (build_with_output) {
            const auto source = toro::load_source_file(argv[2]);
            build_native(source.contents, argv[4]);
        } else if (argc == 3) {
            const auto source = toro::load_source_file(argv[2]);
            if (std::string_view(argv[1]) == "tokens") {
                print_tokens(source.contents);
            } else if (std::string_view(argv[1]) == "ast-expression") {
                print_expression_ast(source.contents);
            } else if (std::string_view(argv[1]) == "check") {
                check_source(source.contents);
            } else if (std::string_view(argv[1]) == "emit-c") {
                emit_c(source.contents);
            } else if (std::string_view(argv[1]) == "build") {
                const auto output = std::filesystem::current_path()
                    / std::filesystem::path(argv[2]).stem();
                build_native(source.contents, output);
            } else if (std::string_view(argv[1]) == "run") {
                return run_native(source.contents);
            } else {
                print_ast(source.contents);
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
