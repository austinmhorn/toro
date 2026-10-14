#include "toro/CGenerator.hpp"

#include "toro/EffectAnalyzer.hpp"
#include "toro/IRLowerer.hpp"

namespace toro {

std::string CGenerator::generate(const Program& program)
{
    auto ir = IRLowerer{}.lower(program);
    EffectAnalyzer{}.analyze(ir);
    return generate(ir);
}

} // namespace toro
