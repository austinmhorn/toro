#pragma once

#include "toro/IR.hpp"

namespace toro {

class EffectAnalyzer {
public:
    void analyze(IRProgram& program) const;
};

} // namespace toro
