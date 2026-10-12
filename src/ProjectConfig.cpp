#include "toro/ProjectConfig.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace toro {
namespace {

constexpr std::string_view manifest_filename = "toro.toml";

[[noreturn]] void throw_manifest_error(
    const std::filesystem::path& path,
    std::size_t line,
    const std::string& message)
{
    std::string location = path.string();
    if (line != 0) {
        location += ":" + std::to_string(line);
    }
    throw std::runtime_error(
        location + ": project manifest error: " + message);
}

std::string_view trim(std::string_view text)
{
    while (!text.empty()
        && std::isspace(static_cast<unsigned char>(text.front()))) {
        text.remove_prefix(1);
    }
    while (!text.empty()
        && std::isspace(static_cast<unsigned char>(text.back()))) {
        text.remove_suffix(1);
    }
    return text;
}

std::string_view without_comment(std::string_view line)
{
    bool quoted = false;
    for (std::size_t index = 0; index < line.size(); ++index) {
        if (line[index] == '"') {
            quoted = !quoted;
        } else if (line[index] == '#' && !quoted) {
            return line.substr(0, index);
        }
    }
    return line;
}

std::string parse_string(
    const std::filesystem::path& path,
    std::size_t line,
    std::string_view value)
{
    value = trim(value);
    if (value.size() < 2 || value.front() != '"' || value.back() != '"') {
        throw_manifest_error(path, line, "values must be double-quoted strings");
    }
    value.remove_prefix(1);
    value.remove_suffix(1);
    if (value.find('"') != std::string_view::npos
        || value.find('\\') != std::string_view::npos) {
        throw_manifest_error(path, line,
            "string escapes and embedded quotes are not supported");
    }
    return std::string(value);
}

std::vector<std::string> parse_string_array(
    const std::filesystem::path& path,
    std::size_t line,
    std::string_view value)
{
    value = trim(value);
    if (value.size() < 2 || value.front() != '[' || value.back() != ']') {
        throw_manifest_error(path, line,
            "ffi.libraries must be an array of double-quoted library names");
    }
    value.remove_prefix(1);
    value.remove_suffix(1);
    std::vector<std::string> result;
    while (!trim(value).empty()) {
        value = trim(value);
        if (value.front() != '"') {
            throw_manifest_error(path, line,
                "ffi.libraries entries must be double-quoted strings");
        }
        const std::size_t closing = value.find('"', 1);
        if (closing == std::string_view::npos) {
            throw_manifest_error(path, line,
                "unterminated string in ffi.libraries");
        }
        result.push_back(parse_string(path, line, value.substr(0, closing + 1)));
        value.remove_prefix(closing + 1);
        value = trim(value);
        if (value.empty()) break;
        if (value.front() != ',') {
            throw_manifest_error(path, line,
                "expected ',' between ffi.libraries entries");
        }
        value.remove_prefix(1);
        if (trim(value).empty()) {
            throw_manifest_error(path, line,
                "ffi.libraries cannot end with a trailing comma");
        }
    }
    return result;
}

bool valid_library_name(std::string_view name)
{
    if (name.empty() || name.front() == '-') return false;
    return std::ranges::all_of(name, [](char character) {
        return std::isalnum(static_cast<unsigned char>(character))
            || character == '_' || character == '+'
            || character == '.' || character == '-';
    });
}

bool valid_project_name(std::string_view name)
{
    const auto ascii_alnum = [](char character) {
        return (character >= 'a' && character <= 'z')
            || (character >= 'A' && character <= 'Z')
            || (character >= '0' && character <= '9');
    };
    if (name.empty() || !ascii_alnum(name.front())) {
        return false;
    }
    for (const char character : name) {
        if (!ascii_alnum(character)
            && character != '-' && character != '_') {
            return false;
        }
    }
    return true;
}

bool path_is_within(
    const std::filesystem::path& child,
    const std::filesystem::path& parent)
{
    const auto relative = std::filesystem::relative(child, parent);
    return !relative.empty() && !relative.is_absolute()
        && *relative.begin() != "..";
}

std::filesystem::path require_relative_path(
    const std::filesystem::path& manifest,
    std::string_view field,
    const std::string& value)
{
    const std::filesystem::path path(value);
    if (value.empty() || path.is_absolute()) {
        throw_manifest_error(manifest, 0,
            std::string(field) + " must be a non-empty relative path");
    }
    for (const auto& component : path) {
        if (component == "..") {
            throw_manifest_error(manifest, 0,
                std::string(field) + (field == "source.entry"
                        ? " must remain inside the configured source root"
                        : " must remain inside the project root"));
        }
    }
    return path.lexically_normal();
}

} // namespace

ProjectConfig ProjectConfigLoader::load(
    const std::filesystem::path& manifest_path) const
{
    const auto absolute_manifest = std::filesystem::absolute(manifest_path)
                                       .lexically_normal();
    std::ifstream input(absolute_manifest);
    if (!input) {
        throw std::runtime_error(
            "project manifest not found at '" + absolute_manifest.string() + "'");
    }

    std::string section;
    std::unordered_set<std::string> sections;
    std::unordered_map<std::string, std::string> values;
    std::vector<std::string> c_libraries;
    bool saw_c_libraries = false;
    std::string line_text;
    std::size_t line_number = 0;
    while (std::getline(input, line_text)) {
        ++line_number;
        const std::string_view line = trim(without_comment(line_text));
        if (line.empty()) {
            continue;
        }
        if (line.front() == '[') {
            if (line.size() < 3 || line.back() != ']') {
                throw_manifest_error(
                    absolute_manifest, line_number, "malformed section header");
            }
            section = std::string(trim(line.substr(1, line.size() - 2)));
            if (section != "project" && section != "source" && section != "ffi") {
                throw_manifest_error(absolute_manifest, line_number,
                    "unknown section '[" + section + "]'");
            }
            if (!sections.insert(section).second) {
                throw_manifest_error(absolute_manifest, line_number,
                    "duplicate section '[" + section + "]'");
            }
            continue;
        }
        if (section.empty()) {
            throw_manifest_error(absolute_manifest, line_number,
                "configuration keys must appear inside a section");
        }
        const std::size_t equals = line.find('=');
        if (equals == std::string_view::npos) {
            throw_manifest_error(
                absolute_manifest, line_number, "expected key = \"value\"");
        }
        const std::string key(trim(line.substr(0, equals)));
        const std::string qualified = section + "." + key;
        if (qualified == "ffi.libraries") {
            if (saw_c_libraries) {
                throw_manifest_error(absolute_manifest, line_number,
                    "duplicate configuration key 'ffi.libraries'");
            }
            c_libraries = parse_string_array(
                absolute_manifest, line_number, line.substr(equals + 1));
            saw_c_libraries = true;
            continue;
        }
        if (qualified != "project.name" && qualified != "source.root"
            && qualified != "source.entry") {
            throw_manifest_error(absolute_manifest, line_number,
                "unknown configuration key '" + qualified + "'");
        }
        if (values.contains(qualified)) {
            throw_manifest_error(absolute_manifest, line_number,
                "duplicate configuration key '" + qualified + "'");
        }
        values.emplace(qualified,
            parse_string(absolute_manifest, line_number, line.substr(equals + 1)));
    }

    for (const std::string required : {
             "project.name", "source.root", "source.entry"}) {
        if (!values.contains(required)) {
            throw_manifest_error(absolute_manifest, 0,
                "missing required configuration key '" + required + "'");
        }
    }
    if (!valid_project_name(values.at("project.name"))) {
        throw_manifest_error(absolute_manifest, 0,
            "project.name must begin with an ASCII letter or digit and contain only letters, digits, '-' or '_'");
    }
    std::unordered_set<std::string> unique_libraries;
    for (const auto& library : c_libraries) {
        if (!valid_library_name(library)) {
            throw_manifest_error(absolute_manifest, 0,
                "ffi.libraries contains invalid library name '" + library
                    + "'; use linker library names without '-l', paths, or flags");
        }
        if (!unique_libraries.insert(library).second) {
            throw_manifest_error(absolute_manifest, 0,
                "ffi.libraries contains duplicate library '" + library + "'");
        }
    }

    const auto project_root = absolute_manifest.parent_path();
    const auto source_relative = require_relative_path(
        absolute_manifest, "source.root", values.at("source.root"));
    const auto entry_relative = require_relative_path(
        absolute_manifest, "source.entry", values.at("source.entry"));
    const auto source_root = (project_root / source_relative).lexically_normal();
    const auto entry_path = (source_root / entry_relative).lexically_normal();
    if (!std::filesystem::is_directory(source_root)) {
        throw_manifest_error(absolute_manifest, 0,
            "source root does not exist or is not a directory: '"
                + source_root.string() + "'");
    }
    if (entry_path.extension() != ".toro") {
        throw_manifest_error(absolute_manifest, 0,
            "source.entry must name a .toro source file");
    }
    if (!std::filesystem::is_regular_file(entry_path)) {
        throw_manifest_error(absolute_manifest, 0,
            "entry source does not exist: '" + entry_path.string() + "'");
    }
    const auto canonical_project = std::filesystem::weakly_canonical(project_root);
    const auto canonical_source = std::filesystem::weakly_canonical(source_root);
    const auto canonical_entry = std::filesystem::weakly_canonical(entry_path);
    if (!path_is_within(canonical_source, canonical_project)) {
        throw_manifest_error(absolute_manifest, 0,
            "source.root resolves outside the project root");
    }
    if (!path_is_within(canonical_entry, canonical_source)) {
        throw_manifest_error(absolute_manifest, 0,
            "source.entry resolves outside the configured source root");
    }

    return ProjectConfig{
        values.at("project.name"),
        absolute_manifest,
        project_root,
        source_root,
        entry_path,
        std::move(c_libraries),
    };
}

ProjectConfig ProjectConfigLoader::discover(
    const std::filesystem::path& directory) const
{
    const auto root = std::filesystem::absolute(directory).lexically_normal();
    return load(root / manifest_filename);
}

} // namespace toro
