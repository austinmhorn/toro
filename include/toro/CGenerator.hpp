#pragma once

#include "toro/IR.hpp"

#include <string>

namespace toro {

struct Program;

class CGenerator {
public:
    [[nodiscard]] std::string generate(const IRProgram& program);

    // Transitional convenience entry point. It performs the same typed-AST to
    // IR lowering, effect analysis, and ARC materialization/optimization as the
    // compiler pipeline before entering the IR backend.
    [[nodiscard]] std::string generate(const Program& program);
};

} // namespace toro
