#pragma once

#include <cctype>
#include <string>
#include <string_view>

namespace toro {

// Produces a stable, path-independent identity for a declaration. The length
// prefixes make dotted module segments and source identifiers unambiguous.
[[nodiscard]] inline std::string declaration_identity(
    std::string_view module_name,
    std::string_view declaration_name)
{
    if (module_name.empty()) {
        return std::string(declaration_name);
    }
    std::string identity = "module";
    std::size_t start = 0;
    while (start <= module_name.size()) {
        const std::size_t end = module_name.find('.', start);
        const auto segment = module_name.substr(
            start, end == std::string_view::npos ? module_name.size() - start
                                                  : end - start);
        identity += "__" + std::to_string(segment.size()) + "_";
        identity.append(segment);
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    identity += "__" + std::to_string(declaration_name.size()) + "_";
    identity.append(declaration_name);
    return identity;
}

[[nodiscard]] inline std::string_view declaration_source_name(
    std::string_view identity)
{
    if (!identity.starts_with("module__")) {
        return identity;
    }
    const std::size_t separator = identity.rfind("__");
    if (separator == std::string_view::npos) {
        return identity;
    }
    const std::size_t underscore = identity.find('_', separator + 2);
    if (underscore == std::string_view::npos) {
        return identity;
    }
    const auto length_text = identity.substr(
        separator + 2, underscore - (separator + 2));
    std::size_t length = 0;
    if (length_text.empty()) {
        return identity;
    }
    for (const char character : length_text) {
        if (!std::isdigit(static_cast<unsigned char>(character))) {
            return identity;
        }
        length = length * 10 + static_cast<std::size_t>(character - '0');
    }
    const auto source_name = identity.substr(underscore + 1);
    return source_name.size() == length ? source_name : identity;
}

} // namespace toro
