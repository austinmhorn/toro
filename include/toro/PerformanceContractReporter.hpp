#pragma once

#include "toro/IR.hpp"

#include <string>

namespace toro {

class PerformanceContractReporter {
public:
    void annotate_diagnostics(IRProgram& program) const;
    [[nodiscard]] std::string report(const IRProgram& program) const;
};

} // namespace toro
