#include "toro/CGenerator.hpp"

#include "toro/ArcOptimizer.hpp"
#include "toro/BorrowInferenceAnalyzer.hpp"
#include "toro/EffectAnalyzer.hpp"
#include "toro/EscapeAnalyzer.hpp"
#include "toro/IRLowerer.hpp"
#include "toro/OwnershipSummaryAnalyzer.hpp"
#include "toro/PerformanceContractAnalyzer.hpp"
#include "toro/PerformanceContractReporter.hpp"
#include "toro/StorageDecisionAnalyzer.hpp"
#include "toro/StrongCycleAnalyzer.hpp"

#include <stdexcept>
#include <string>

namespace toro {

std::string CGenerator::generate(const Program& program)
{
    auto ir = IRLowerer{}.lower(program);
    EffectAnalyzer{}.analyze(ir);
    OwnershipSummaryAnalyzer{}.analyze(ir);
    StrongCycleAnalyzer{}.analyze(ir);
    EscapeAnalyzer{}.analyze(ir);
    StorageDecisionAnalyzer{}.analyze(ir);
    ArcMaterializer{}.materialize(ir);
    OwnershipTransferAnalyzer{}.analyze(ir);
    BorrowInferenceAnalyzer{}.analyze(ir);
    ArcOptimizer{}.optimize(ir);
    PerformanceContractAnalyzer{}.analyze(ir);
    PerformanceContractReporter{}.annotate_diagnostics(ir);
    if (has_error_diagnostics(ir.diagnostics)) {
        std::string message;
        for (const auto& diagnostic : ir.diagnostics) {
            if (diagnostic.severity == DiagnosticSeverity::Error) {
                message += format_diagnostic(diagnostic);
            }
        }
        throw std::runtime_error(message);
    }
    return generate(ir);
}

} // namespace toro
