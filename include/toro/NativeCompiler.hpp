#pragma once

#include <filesystem>
#include <string_view>

namespace toro {

class NativeCompiler {
public:
    NativeCompiler();

    void build(
        std::string_view c_source,
        const std::filesystem::path& output_path) const;

    [[nodiscard]] int run(std::string_view c_source) const;

    [[nodiscard]] const std::filesystem::path& compiler_path() const;

private:
    std::filesystem::path compiler_path_;
};

} // namespace toro
