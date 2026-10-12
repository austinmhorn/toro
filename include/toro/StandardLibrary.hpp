#pragma once

#include <optional>
#include <string_view>
#include <vector>

namespace toro {

enum class RuntimeIntrinsic {
    IoWriteLine,
    FsExists,
    FsReadText,
    FsWriteText,
    FsRemoveFile,
    EnvHas,
    EnvGet,
    ProcessExit,
    TimeWallMillis,
    StringLength,
    StringConcat,
    StringContains,
    StringStartsWith,
    StringEndsWith,
    StringSubstring,
};

[[nodiscard]] std::optional<std::string_view> standard_library_source(
    std::string_view module_name);
[[nodiscard]] bool is_standard_library_module(std::string_view module_name);
[[nodiscard]] const std::vector<std::string_view>& runtime_intrinsic_names();
[[nodiscard]] std::optional<RuntimeIntrinsic> runtime_intrinsic(
    std::string_view name);

} // namespace toro
