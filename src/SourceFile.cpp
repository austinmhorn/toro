#include "toro/SourceFile.hpp"

#include <fstream>
#include <iterator>
#include <stdexcept>
#include <utility>

namespace toro {

SourceFile load_source_file(const std::filesystem::path& path)
{
    if (path.extension() != ".to") {
        throw std::invalid_argument("toro source files must use the .to extension");
    }

    if (!std::filesystem::exists(path)) {
        throw std::runtime_error("source file does not exist: " + path.string());
    }

    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("could not open source file: " + path.string());
    }

    std::string contents{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};

    if (input.bad()) {
        throw std::runtime_error("could not read source file: " + path.string());
    }

    return SourceFile{path, std::move(contents)};
}

} // namespace toro
