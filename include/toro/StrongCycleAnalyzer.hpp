#pragma once

#include "toro/IR.hpp"

namespace toro {

class StrongCycleAnalyzer {
public:
    void analyze(IRProgram& program) const;
};

} // namespace toro
