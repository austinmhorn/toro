#include "toro/TestRunner.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace {

void expect(bool condition, std::string_view message)
{
    if (!condition) throw std::runtime_error(std::string(message));
}

class TestProject {
public:
    TestProject()
    {
#if defined(_WIN32)
        root_ = std::filesystem::temp_directory_path() / "toro-test-runner-tests";
#else
        root_ = std::filesystem::temp_directory_path()
            / ("toro-test-runner-tests-" + std::to_string(::getpid()));
#endif
        std::filesystem::remove_all(root_);
        std::filesystem::create_directories(root_ / "src/tests");
        write("src/main.toro", "function main() {}\n");
    }

    ~TestProject() { std::filesystem::remove_all(root_); }

    void write(std::string_view relative, std::string_view contents) const
    {
        const auto path = root_ / relative;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary);
        output << contents;
    }

    [[nodiscard]] toro::ProjectConfig config() const
    {
        return toro::ProjectConfig{
            "test-project",
            root_ / "toro.toml",
            root_,
            root_ / "src",
            root_ / "src/main.toro"};
    }

private:
    std::filesystem::path root_;
};

void test_discovery_order_imports_and_stdlib()
{
    TestProject project;
    project.write("src/util/math.toro",
        "public function twice(value: int) -> int { return value * 2 }\n");
    project.write("src/tests/zeta.toro",
        "import util.math\n"
        "function test_zeta() -> bool { return twice(3) == 6 }\n");
    project.write("src/tests/alpha.toro",
        "import strings\n"
        "function test_second() -> bool { return contains(\"Toro\", \"or\") }\n"
        "function test_first() -> bool { return length(\"Toro\") == 4 }\n");

    std::ostringstream output;
    expect(toro::TestRunner().run(project.config(), output) == 0,
        "passing test project failed");
    const std::string report = output.str();
    const auto first = report.find("tests/alpha.toro::test_first");
    const auto second = report.find("tests/alpha.toro::test_second");
    const auto zeta = report.find("tests/zeta.toro::test_zeta");
    expect(first < second && second < zeta,
        "test discovery/reporting order was not deterministic");
    expect(report.find("3 passed, 0 failed") != std::string::npos,
        "passing summary was incorrect");
}

void test_failure_and_compile_diagnostics()
{
    {
        TestProject project;
        project.write("src/tests/failure.toro",
            "function test_false() -> bool { return false }\n");
        std::ostringstream output;
        expect(toro::TestRunner().run(project.config(), output) == 1,
            "false test did not fail the run");
        expect(output.str().find("exited with status 1") != std::string::npos,
            "false test did not report its exit status");
    }
    {
        TestProject project;
        project.write("src/tests/compile.toro",
            "function test_compile() -> bool { return missing == 1 }\n");
        std::ostringstream output;
        expect(toro::TestRunner().run(project.config(), output) == 1,
            "test compile error did not fail the run");
        expect(output.str().find("unknown identifier") != std::string::npos,
            "test compile error was not reported");
    }
    {
        TestProject project;
        project.write("src/tests/signature.toro",
            "function test_bad(value: int) -> bool { return true }\n");
        std::ostringstream output;
        expect(toro::TestRunner().run(project.config(), output) == 1,
            "invalid test signature did not fail discovery");
        expect(output.str().find("take no parameters") != std::string::npos,
            "invalid test signature diagnostic was unclear");
        expect(output.str().find("0 passed, 1 failed") != std::string::npos,
            "discovery failure summary was incorrect");
    }
}

void test_no_tests_is_an_error()
{
    TestProject project;
    std::ostringstream output;
    expect(toro::TestRunner().run(project.config(), output) == 1,
        "empty test project succeeded");
    expect(output.str().find("no tests found") != std::string::npos,
        "empty test project diagnostic was unclear");
}

} // namespace

int main()
{
    try {
        test_discovery_order_imports_and_stdlib();
        test_failure_and_compile_diagnostics();
        test_no_tests_is_an_error();
    } catch (const std::exception& error) {
        std::cerr << "test runner test failure: " << error.what() << '\n';
        return 1;
    }
    std::cout << "test runner tests passed\n";
    return 0;
}
