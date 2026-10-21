#include "toro/Diagnostic.hpp"

#include <string>

namespace toro {
namespace {

std::string source_text(const DiagnosticSource& source)
{
    std::string result;
    if (!source.module_name.empty()) {
        result = source.module_name + ':';
    }
    result += std::to_string(source.location.line) + ':'
        + std::to_string(source.location.column);
    return result;
}

} // namespace

std::string_view diagnostic_severity_name(DiagnosticSeverity severity)
{
    switch (severity) {
    case DiagnosticSeverity::Warning: return "warning";
    }
    return "diagnostic";
}

std::string format_diagnostic(const Diagnostic& diagnostic)
{
    std::string result(diagnostic_severity_name(diagnostic.severity));
    result += ": " + diagnostic.message;
    if (!diagnostic.code.empty()) {
        result += " [" + diagnostic.code + ']';
    }
    if (diagnostic.source) {
        result += "\n  --> " + source_text(*diagnostic.source);
    }
    for (const auto& note : diagnostic.notes) {
        result += "\n  note: " + note.message;
        if (note.source) {
            result += " (declared at " + source_text(*note.source) + ')';
        }
    }
    result += '\n';
    return result;
}

} // namespace toro
