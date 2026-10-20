#pragma once

#include "toro/IR.hpp"

namespace toro {

class EscapeAnalyzer {
public:
    void analyze(IRProgram& program) const;
};

} // namespace toro
