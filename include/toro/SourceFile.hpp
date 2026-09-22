#pragma once

#include <filesystem>
#include <string>

namespace toro {

struct SourceFile {
    std::filesystem::path path;
    std::string contents;
};

[[nodiscard]] SourceFile load_source_file(const std::filesystem::path& path);

} // namespace toro
