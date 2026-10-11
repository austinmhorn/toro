#pragma once

#include "toro/AST.hpp"

#include <filesystem>

namespace toro {

class ModuleLoader {
public:
    [[nodiscard]] Program load(const std::filesystem::path& entry_path) const;
    [[nodiscard]] Program load(
        const std::filesystem::path& entry_path,
        const std::filesystem::path& source_root) const;
};

} // namespace toro
