#pragma once

#include "toro/SourceLocation.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace toro {

enum class DiagnosticSeverity {
    Warning,
};

struct DiagnosticSource {
    std::string module_name;
    SourceLocation location{1, 1};
};

struct DiagnosticNote {
    std::string message;
    std::optional<DiagnosticSource> source;
};

struct Diagnostic {
    DiagnosticSeverity severity{DiagnosticSeverity::Warning};
    std::string code;
    std::string message;
    std::optional<DiagnosticSource> source;
    std::vector<DiagnosticNote> notes;
};

[[nodiscard]] std::string_view diagnostic_severity_name(
    DiagnosticSeverity severity);
[[nodiscard]] std::string format_diagnostic(const Diagnostic& diagnostic);

} // namespace toro
