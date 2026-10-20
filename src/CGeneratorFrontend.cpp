#include "toro/CGenerator.hpp"

#include "toro/ArcOptimizer.hpp"
#include "toro/EffectAnalyzer.hpp"
#include "toro/EscapeAnalyzer.hpp"
#include "toro/IRLowerer.hpp"
#include "toro/StorageDecisionAnalyzer.hpp"

namespace toro {

std::string CGenerator::generate(const Program& program)
{
    auto ir = IRLowerer{}.lower(program);
    EffectAnalyzer{}.analyze(ir);
    EscapeAnalyzer{}.analyze(ir);
    StorageDecisionAnalyzer{}.analyze(ir);
    ArcMaterializer{}.materialize(ir);
    OwnershipTransferAnalyzer{}.analyze(ir);
    ArcOptimizer{}.optimize(ir);
    return generate(ir);
}

} // namespace toro
