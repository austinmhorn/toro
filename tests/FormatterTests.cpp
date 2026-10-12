#include "toro/Formatter.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
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

class TestTree {
public:
    TestTree()
    {
#if defined(_WIN32)
        root_ = std::filesystem::temp_directory_path() / "toro-formatter-tests";
#else
        root_ = std::filesystem::temp_directory_path()
            / ("toro-formatter-tests-" + std::to_string(::getpid()));
#endif
        std::filesystem::remove_all(root_);
        std::filesystem::create_directories(root_);
    }

    ~TestTree() { std::filesystem::remove_all(root_); }

    std::filesystem::path write(
        std::string_view name, std::string_view contents) const
    {
        const auto path = root_ / name;
        std::ofstream output(path, std::ios::binary);
        output << contents;
        return path;
    }

    static std::string read(const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(input), {}};
    }

private:
    std::filesystem::path root_;
};

void test_canonical_formatting_and_idempotence()
{
    constexpr std::string_view source =
        "import strings\n"
        "function main(){\n"
        "x:=10+20*3\n"
        "if x>5{print(\"large\")}else{print(\"small\")}\n"
        "}\n";
    constexpr std::string_view expected =
        "import strings\n\n"
        "function main() {\n"
        "    x := 10 + 20 * 3\n"
        "    if x > 5 {\n"
        "        print(\"large\")\n"
        "    } else {\n"
        "        print(\"small\")\n"
        "    }\n"
        "}\n";
    const toro::Formatter formatter;
    const std::string formatted = formatter.format(source);
    expect(formatted == expected, "basic canonical formatting changed");
    expect(formatter.format(formatted) == formatted,
        "formatter output was not idempotent");
}

void test_current_language_constructs()
{
    constexpr std::string_view source =
        "public enum Outcome{\n"
        "ok(int)\nerror(string)\n}\n"
        "public struct Pair<A,B>{\nfirst:A\nsecond:B\n"
        "function get()->A{return self.first}\n}\n"
        "public class Worker{\npublic name:string\n"
        "init(name:string){self.name=name}\n"
        "public function run<T: Comparable+Serializable>(value:T)->T{return value}\n"
        "overload as string{return self.name}\n"
        "destroy(){print(self.name)}\n}\n"
        "function inspect(result:Outcome){\nhandle result{\n"
        "ok(value){print(value)}\nerror(problem){print(problem)}\n}\n}\n";
    const toro::Formatter formatter;
    const std::string formatted = formatter.format(source);
    expect(formatted.find("public enum Outcome {") != std::string::npos,
        "enum was not formatted");
    expect(formatted.find("init(name: string) {") != std::string::npos,
        "lifecycle declaration was not formatted canonically");
    expect(formatted.find("overload as string {") != std::string::npos,
        "conversion overload was not formatted");
    const std::string expressions = formatter.format(
        "function use(values:List<int>,result:Result<int,string>)->dec{\n"
        "values[0]=values[0]+1\n"
        "outcome:=Outcome::ok(values[0])\n"
        "value:=result?\n"
        "return value as dec\n}\n");
    expect(expressions.find("values[0] = values[0] + 1") != std::string::npos,
        "collection index syntax was not formatted");
    expect(expressions.find("Outcome::ok(values[0])") != std::string::npos,
        "type-scoped access was not formatted");
    expect(expressions.find("value := result?") != std::string::npos,
        "Result propagation was not formatted");
    expect(expressions.find("return value as dec") != std::string::npos,
        "explicit cast was not formatted");
    expect(formatter.format(formatted) == formatted,
        "full-language formatting was not idempotent");
}

void test_check_write_and_parse_safety()
{
    TestTree tree;
    const toro::Formatter formatter;
    const auto source = tree.write("main.toro", "function main(){print(\"Toro\")}\n");
    const std::string original = TestTree::read(source);
    expect(formatter.format_file(source, true),
        "check mode did not detect unformatted source");
    expect(TestTree::read(source) == original, "check mode modified source");
    expect(formatter.format_file(source), "write mode did not report its change");
    expect(!formatter.format_file(source, true),
        "formatted source failed check mode");

    const auto dirty = tree.write("dirty.toro", "function dirty(){print(1)}\n");
    const std::string dirty_original = TestTree::read(dirty);
    const auto invalid = tree.write("invalid.toro", "function main( {\n");
    const std::string invalid_original = TestTree::read(invalid);
    bool failed = false;
    try {
        static_cast<void>(formatter.format_files({dirty, invalid}));
    } catch (const std::exception& error) {
        failed = std::string_view(error.what()).find("cannot format")
            != std::string_view::npos;
    }
    expect(failed, "invalid syntax did not produce a formatter diagnostic");
    expect(TestTree::read(invalid) == invalid_original,
        "formatter changed invalid source");
    expect(TestTree::read(dirty) == dirty_original,
        "batch parse failure partially formatted an earlier source");
}

void test_comments_fail_without_modification()
{
    TestTree tree;
    const auto path = tree.write(
        "comments.toro", "function main() {\n    // preserved later\n}\n");
    const std::string original = TestTree::read(path);
    bool failed = false;
    try {
        static_cast<void>(toro::Formatter().format_file(path));
    } catch (const std::exception&) {
        failed = true;
    }
    expect(failed, "unsupported comments were silently discarded");
    expect(TestTree::read(path) == original,
        "formatter modified a source containing unsupported comments");
}

} // namespace

int main()
{
    try {
        test_canonical_formatting_and_idempotence();
        test_current_language_constructs();
        test_check_write_and_parse_safety();
        test_comments_fail_without_modification();
    } catch (const std::exception& error) {
        std::cerr << "formatter test failure: " << error.what() << '\n';
        return 1;
    }
    std::cout << "formatter tests passed\n";
    return 0;
}
