#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace toro {

struct ExperimentalArenaConfig {
    std::string type_name;
    std::string region_name;
    std::size_t capacity_bytes{0};
    std::size_t max_objects{0};
};

struct ProjectConfig {
    std::string name;
    std::filesystem::path manifest_path;
    std::filesystem::path project_root;
    std::filesystem::path source_root;
    std::filesystem::path entry_path;
    std::vector<std::string> c_libraries;
    std::optional<ExperimentalArenaConfig> experimental_arena;
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
