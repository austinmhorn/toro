#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace toro {

class Formatter {
public:
    [[nodiscard]] std::string format(std::string_view source) const;

    // Returns true when the source differs from canonical formatting. In check
    // mode the file is never modified.
    [[nodiscard]] bool format_file(
        const std::filesystem::path& path,
        bool check_only = false) const;

    // Every source is parsed and formatted before any file is replaced.
    [[nodiscard]] std::vector<std::filesystem::path> format_files(
        const std::vector<std::filesystem::path>& paths,
        bool check_only = false) const;
};

} // namespace toro
