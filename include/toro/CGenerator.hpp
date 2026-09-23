#pragma once

#include "toro/AST.hpp"

#include <string>

namespace toro {

class CGenerator {
public:
    [[nodiscard]] std::string generate(const Program& program);
};

} // namespace toro
