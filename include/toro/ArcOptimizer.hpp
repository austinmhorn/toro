#pragma once

#include "toro/IR.hpp"

namespace toro {

class ArcMaterializer {
public:
    void materialize(IRProgram& program) const;
};

class ArcOptimizer {
public:
    void optimize(IRProgram& program) const;
};

} // namespace toro
