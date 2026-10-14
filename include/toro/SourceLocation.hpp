#pragma once

#include <cstddef>

namespace toro {

struct SourceLocation {
    std::size_t line;
    std::size_t column;
};

} // namespace toro
