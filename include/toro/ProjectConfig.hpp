#pragma once

#include <filesystem>
#include <string>

namespace toro {

struct ProjectConfig {
    std::string name;
    std::filesystem::path manifest_path;
    std::filesystem::path project_root;
    std::filesystem::path source_root;
    std::filesystem::path entry_path;
};

class ProjectConfigLoader {
public:
    [[nodiscard]] ProjectConfig load(
        const std::filesystem::path& manifest_path) const;

    // Discovery is intentionally exact: only <directory>/toro.toml is checked.
    [[nodiscard]] ProjectConfig discover(
        const std::filesystem::path& directory =
            std::filesystem::current_path()) const;
};

} // namespace toro
