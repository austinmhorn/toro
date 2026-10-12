#include "toro/StandardLibrary.hpp"

#include <array>
#include <utility>

namespace toro {
namespace {

constexpr std::string_view io_source = R"toro(
public function write_line(value: string) {
    __toro_io_write_line(value)
}
)toro";

constexpr std::string_view fs_source = R"toro(
public function exists(path: string) -> bool {
    return __toro_fs_exists(path)
}

public function read_text(path: string) -> Result<string, string> {
    return __toro_fs_read_text(path)
}

public function write_text(path: string, contents: string) -> Result<bool, string> {
    return __toro_fs_write_text(path, contents)
}

public function remove_file(path: string) -> Result<bool, string> {
    return __toro_fs_remove_file(path)
}
)toro";

constexpr std::string_view env_source = R"toro(
public function has(name: string) -> bool {
    return __toro_env_has(name)
}

public function get(name: string) -> Result<string, string> {
    return __toro_env_get(name)
}
)toro";

constexpr std::string_view process_source = R"toro(
public function exit(code: int) {
    __toro_process_exit(code)
}
)toro";

constexpr std::string_view time_source = R"toro(
public function wall_millis() -> int {
    return __toro_time_wall_millis()
}

public function elapsed_millis(start: int) -> int {
    return wall_millis() - start
}
)toro";

constexpr std::string_view strings_source = R"toro(
public function length(value: string) -> int {
    return __toro_string_length(value)
}

public function concat(left: string, right: string) -> string {
    return __toro_string_concat(left, right)
}

public function contains(value: string, fragment: string) -> bool {
    return __toro_string_contains(value, fragment)
}

public function starts_with(value: string, prefix: string) -> bool {
    return __toro_string_starts_with(value, prefix)
}

public function ends_with(value: string, suffix: string) -> bool {
    return __toro_string_ends_with(value, suffix)
}

public function substring(value: string, start: int, count: int) -> Result<string, string> {
    return __toro_string_substring(value, start, count)
}
)toro";

constexpr std::array modules{
    std::pair{"env", env_source},
    std::pair{"fs", fs_source},
    std::pair{"io", io_source},
    std::pair{"process", process_source},
    std::pair{"strings", strings_source},
    std::pair{"time", time_source},
};

constexpr std::array intrinsics{
    std::pair{"__toro_io_write_line", RuntimeIntrinsic::IoWriteLine},
    std::pair{"__toro_fs_exists", RuntimeIntrinsic::FsExists},
    std::pair{"__toro_fs_read_text", RuntimeIntrinsic::FsReadText},
    std::pair{"__toro_fs_write_text", RuntimeIntrinsic::FsWriteText},
    std::pair{"__toro_fs_remove_file", RuntimeIntrinsic::FsRemoveFile},
    std::pair{"__toro_env_has", RuntimeIntrinsic::EnvHas},
    std::pair{"__toro_env_get", RuntimeIntrinsic::EnvGet},
    std::pair{"__toro_process_exit", RuntimeIntrinsic::ProcessExit},
    std::pair{"__toro_time_wall_millis", RuntimeIntrinsic::TimeWallMillis},
    std::pair{"__toro_string_length", RuntimeIntrinsic::StringLength},
    std::pair{"__toro_string_concat", RuntimeIntrinsic::StringConcat},
    std::pair{"__toro_string_contains", RuntimeIntrinsic::StringContains},
    std::pair{"__toro_string_starts_with", RuntimeIntrinsic::StringStartsWith},
    std::pair{"__toro_string_ends_with", RuntimeIntrinsic::StringEndsWith},
    std::pair{"__toro_string_substring", RuntimeIntrinsic::StringSubstring},
};

} // namespace

std::optional<std::string_view> standard_library_source(
    std::string_view module_name)
{
    for (const auto& [name, source] : modules) {
        if (name == module_name) {
            return source;
        }
    }
    return std::nullopt;
}

bool is_standard_library_module(std::string_view module_name)
{
    return standard_library_source(module_name).has_value();
}

const std::vector<std::string_view>& runtime_intrinsic_names()
{
    static const std::vector<std::string_view> names = [] {
        std::vector<std::string_view> result;
        result.reserve(intrinsics.size());
        for (const auto& [name, unused] : intrinsics) {
            static_cast<void>(unused);
            result.push_back(name);
        }
        return result;
    }();
    return names;
}

std::optional<RuntimeIntrinsic> runtime_intrinsic(std::string_view name)
{
    for (const auto& [candidate, intrinsic] : intrinsics) {
        if (candidate == name) {
            return intrinsic;
        }
    }
    return std::nullopt;
}

} // namespace toro
