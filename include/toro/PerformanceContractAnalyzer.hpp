#pragma once

#include "toro/IR.hpp"

namespace toro {

class PerformanceContractAnalyzer {
public:
    void analyze(IRProgram& program) const;
};

} // namespace toro
