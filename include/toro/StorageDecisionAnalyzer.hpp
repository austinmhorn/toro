#pragma once

#include "toro/IR.hpp"

#include <cstddef>
#include <string>

namespace toro {

struct ExperimentalArenaOptions {
    bool enabled{false};
    std::string type_name;
    std::string region_name{"tick"};
    std::size_t capacity_bytes{0};
    std::size_t max_objects{0};
};

class StorageDecisionAnalyzer {
public:
    void analyze(
        IRProgram& program,
        bool enable_stack = true,
        ExperimentalArenaOptions arena = {}) const;
};

} // namespace toro
