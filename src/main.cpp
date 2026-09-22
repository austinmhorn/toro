#include "toro/SourceFile.hpp"

#include <exception>
#include <iostream>
#include <string_view>

namespace {

void print_usage(std::ostream& output)
{
    output << "Usage: toro <source.to>\n"
              "       toro --version\n";
}

} // namespace

int main(int argc, char* argv[])
{
    if (argc == 2 && std::string_view(argv[1]) == "--version") {
        std::cout << "toro compiler v0.1.0\n";
        return 0;
    }

    if (argc != 2) {
        if (argc > 2) {
            std::cerr << "error: too many arguments\n";
        }
        print_usage(std::cerr);
        return 1;
    }

    try {
        const auto source = toro::load_source_file(argv[1]);
        std::cout << source.contents;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }

    return 0;
}
