#pragma once

#include "toro/IR.hpp"

namespace toro {

class StorageDecisionAnalyzer {
public:
    void analyze(IRProgram& program, bool enable_stack = true) const;
};

} // namespace toro
