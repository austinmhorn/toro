#include "toro/Lexer.hpp"
#include "toro/SourceFile.hpp"
#include "toro/Token.hpp"

#include <exception>
#include <iostream>
#include <string_view>

namespace {

void print_usage(std::ostream& output)
{
    output << "Usage: toro <source.to>\n"
              "       toro tokens <source.to>\n"
              "       toro --version\n";
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

} // namespace

int main(int argc, char* argv[])
{
    if (argc == 2 && std::string_view(argv[1]) == "--version") {
        std::cout << "toro compiler v0.1.0\n";
        return 0;
    }

    if (argc == 2 && std::string_view(argv[1]) == "tokens") {
        std::cerr << "error: tokens requires a source file\n";
        print_usage(std::cerr);
        return 1;
    }

    if (argc != 2 && !(argc == 3 && std::string_view(argv[1]) == "tokens")) {
        std::cerr << "error: invalid arguments\n";
        print_usage(std::cerr);
        return 1;
    }

    try {
        if (argc == 3) {
            const auto source = toro::load_source_file(argv[2]);
            print_tokens(source.contents);
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
