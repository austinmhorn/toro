#pragma once

#include "toro/IR.hpp"

namespace toro {

class BorrowInferenceAnalyzer {
public:
    void analyze(IRProgram& program, bool enable_borrowing = true) const;
};

} // namespace toro
