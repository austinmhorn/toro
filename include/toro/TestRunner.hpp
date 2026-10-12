#pragma once

#include "toro/ProjectConfig.hpp"

#include <iosfwd>

namespace toro {

class TestRunner {
public:
    // Returns zero when every discovered test passes. Tests are zero-argument
    // functions named test_* with an explicit bool return type.
    [[nodiscard]] int run(
        const ProjectConfig& project,
        std::ostream& output) const;
};

} // namespace toro
