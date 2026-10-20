#include "toro/CGenerator.hpp"

#include "toro/ArcOptimizer.hpp"
#include "toro/EffectAnalyzer.hpp"
#include "toro/IRLowerer.hpp"

namespace toro {

std::string CGenerator::generate(const Program& program)
{
    auto ir = IRLowerer{}.lower(program);
    EffectAnalyzer{}.analyze(ir);
    ArcMaterializer{}.materialize(ir);
    OwnershipTransferAnalyzer{}.analyze(ir);
    ArcOptimizer{}.optimize(ir);
    return generate(ir);
}

} // namespace toro
