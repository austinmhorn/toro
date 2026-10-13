#pragma once

#include "toro/AST.hpp"
#include "toro/IR.hpp"

namespace toro {

class IRLowerer {
public:
    [[nodiscard]] IRProgram lower(const Program& program) const;
};

} // namespace toro
