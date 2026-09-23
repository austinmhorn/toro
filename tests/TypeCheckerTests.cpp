#include "toro/Lexer.hpp"
#include "toro/Parser.hpp"
#include "toro/SemanticAnalyzer.hpp"
#include "toro/TypeChecker.hpp"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

void check(std::string_view source)
{
    auto tokens = toro::Lexer(source).tokenize();
    const auto program = toro::Parser(std::move(tokens)).parse_program();
    toro::SemanticAnalyzer().analyze(program);
    toro::TypeChecker().check(program);
}

void expect_valid(std::string_view source)
{
    check(source);
}

void expect_error(std::string_view source, std::string_view expected_message)
{
    try {
        check(source);
    } catch (const std::runtime_error& error) {
        if (std::string_view(error.what()).find(expected_message) == std::string_view::npos) {
            throw std::runtime_error(
                "expected type error containing '" + std::string(expected_message)
                + "', got '" + error.what() + "'");
        }
        return;
    }
    throw std::runtime_error("expected type-checking failure");
}

void test_primitive_inference_and_explicit_types()
{
    expect_valid(
        "integer := 10\n"
        "decimal := 19.99\n"
        "name := \"toro\"\n"
        "active := true\n"
        "print(integer)\n"
        "print(decimal)\n"
        "print(name)\n"
        "print(active)\n");
    expect_valid(
        "integer: int = 10\n"
        "decimal: dec = 19.99\n"
        "name: string = \"toro\"\n"
        "active: bool = false\n");
    expect_error(
        "value: int = \"hello\"\n",
        "cannot assign value of type 'string' to type 'int'");
}

void test_assignment_types()
{
    expect_valid("value := 10\nvalue = 20\n");
    expect_error(
        "value := 10\nvalue = \"hello\"\n",
        "cannot assign value of type 'string' to type 'int'");
    expect_valid(
        "value := 10\n"
        "{\n"
        "    value := \"shadow\"\n"
        "    value = \"updated\"\n"
        "}\n"
        "value = 20\n");
}

void test_arithmetic_and_comparisons()
{
    expect_valid(
        "integer := -10 + 20 * 3 / 2\n"
        "decimal := 1.5 + 2.5\n"
        "less := integer < 100\n"
        "greater := decimal >= 1.0\n"
        "equal := integer == 20\n"
        "different := decimal != 0.0\n");
    expect_error("value := \"hello\" + 1\n", "requires numeric operands");
    expect_error("value := -\"hello\"\n", "unary '-' requires a numeric operand");
    expect_error("value := 1 + 2.0\n", "does not implicitly convert 'int' and 'dec'");
    expect_error("value := true < false\n", "comparison requires numeric operands");
    expect_error("value := 1 == \"1\"\n", "equality operands must have the same type");
}

void test_logical_operators_and_conditions()
{
    expect_valid(
        "active := true\n"
        "ready := false\n"
        "result := active and ready or true\n"
        "if result {\n"
        "    print(result)\n"
        "}\n"
        "while active {\n"
        "    stop\n"
        "}\n");
    expect_error("value := true and 1\n", "requires bool operands");
    expect_error("value := 1 or false\n", "requires bool operands");
    expect_error("if 1 {}\n", "condition must have type 'bool'");
    expect_error("while \"yes\" {}\n", "condition must have type 'bool'");
}

void test_function_arguments()
{
    expect_valid(
        "function main() {\n"
        "    result := add(1, 2)\n"
        "    print(result)\n"
        "}\n"
        "function add(a: int, b: int) -> int {\n"
        "    return a + b\n"
        "}\n");
    expect_error(
        "function add(a: int, b: int) -> int { return a + b }\n"
        "value := add(1)\n",
        "function 'add' expects 2 arguments, got 1");
    expect_error(
        "function echo(value: string) -> string { return value }\n"
        "value := echo(10)\n",
        "argument 1 to 'echo' expects 'string', got 'int'");
    expect_valid(
        "function combine(number: int, text: string) -> string { return text }\n"
        "value := combine(text: \"toro\", number: 10)\n");
    expect_error(
        "function combine(number: int, text: string) -> string { return text }\n"
        "value := combine(text: 10, number: 20)\n",
        "argument 1 to 'combine' expects 'string', got 'int'");
    expect_error(
        "function echo(value: string) -> string { return value }\n"
        "value := echo(other: \"toro\")\n",
        "function 'echo' has no parameter named 'other'");
    expect_error(
        "function nothing() {}\n"
        "value := nothing()\n",
        "expression does not produce a value");
    expect_error(
        "function action() {}\n"
        "function main() {\n"
        "    action := 10\n"
        "    action()\n"
        "}\n",
        "value 'action' of type 'int' is not callable");
}

void test_function_returns()
{
    expect_valid(
        "function value() -> int { return 10 }\n"
        "function action() { return }\n"
        "function unchecked_path() -> bool {}\n");
    expect_error(
        "function value() -> int { return }\n",
        "return requires a value of type 'int'");
    expect_error(
        "function action() { return 10 }\n",
        "a function without a return type cannot return a value");
    expect_error(
        "function value() -> int { return \"wrong\" }\n",
        "cannot assign value of type 'string' to type 'int'");
}

void test_null_restrictions()
{
    expect_valid("print(null)\n");
    expect_error(
        "value := null\n",
        "cannot infer a variable type from null without nullable types");
    expect_error("value: int = null\n", "null requires a nullable type");
    expect_error(
        "value := 10\nvalue = null\n",
        "null requires a nullable type");
    expect_error(
        "value := 10 == null\n",
        "null cannot be compared with non-null type 'int'");
}

void test_nullable_declarations_and_assignments()
{
    expect_valid(
        "name: string? = null\n"
        "name = \"toro\"\n"
        "other: string = \"compiler\"\n"
        "name = other\n"
        "number: int? = 10\n"
        "number = null\n");
    expect_valid(
        "class User {}\n"
        "user: User? = null\n"
        "actual: User = User()\n"
        "user = actual\n"
        "items: List<User>? = null\n"
        "nested: Map<string, List<User?>>? = null\n");
    expect_error(
        "maybe: string? = \"toro\"\n"
        "required: string = maybe\n",
        "cannot assign value of type 'string?' to type 'string'");
    expect_error(
        "class User {}\n"
        "maybe: User? = User()\n"
        "required: User = maybe\n",
        "cannot assign value of type 'User?' to type 'User'");
    expect_error(
        "class User {}\n"
        "class Team {}\n"
        "user: User? = Team()\n",
        "cannot assign value of type 'Team' to type 'User?'");
    expect_error(
        "class User {}\n"
        "user: User? = 10\n",
        "cannot assign value of type 'int' to type 'User?'");
}

void test_nullable_functions_and_equality()
{
    expect_valid(
        "class User {}\n"
        "function find_user(id: int) -> User? {\n"
        "    return null\n"
        "}\n"
        "function default_name() -> string? { return \"toro\" }\n"
        "function accept(user: User?) {\n"
        "    missing := user == null\n"
        "    present := user != null\n"
        "    print(missing)\n"
        "    print(present)\n"
        "}\n"
        "function main() {\n"
        "    user: User? = find_user(1)\n"
        "    accept(user)\n"
        "    accept(User())\n"
        "    accept(null)\n"
        "}\n");
    expect_error(
        "class User {}\n"
        "function find_user() -> User { return null }\n",
        "null requires a nullable type");
    expect_error(
        "function require_name(name: string?) -> string { return name }\n",
        "cannot assign value of type 'string?' to type 'string'");
    expect_error(
        "class User {}\n"
        "function require_user(user: User) {}\n"
        "require_user(null)\n",
        "got 'null'");
    expect_error(
        "class User {}\n"
        "function require_user(user: User) {}\n"
        "maybe: User? = null\n"
        "require_user(maybe)\n",
        "expects 'User', got 'User?'");
    expect_error(
        "class User {}\n"
        "user: User = User()\n"
        "missing := user == null\n",
        "null cannot be compared with non-null type 'User'");
    expect_error(
        "name: string? = null\n"
        "if name {}\n",
        "condition must have type 'bool', got nullable type 'string?'");
}

void test_builtin_casts()
{
    expect_valid(
        "decimal := 10 as dec\n"
        "integer := 19.9 as int\n"
        "same := integer as int\n"
        "sum := integer as dec + 1.0\n"
        "function accept(value: dec) {}\n"
        "accept(10 as dec)\n");
    expect_error(
        "value := \"10\" as int\n",
        "no conversion from 'string' to 'int'");
    expect_error(
        "value: int? = 10\n"
        "required := value as int\n",
        "does not unwrap the value");
    expect_valid(
        "value: int = 10\n"
        "optional := value as int?\n");
}

void test_user_defined_conversions()
{
    expect_valid(
        "class Player {\n"
        "    public name: string\n"
        "    overload as string { return self.name }\n"
        "}\n"
        "function main() {\n"
        "    player := Player()\n"
        "    name := player as string\n"
        "    print(name)\n"
        "    print(player)\n"
        "}\n");
    expect_valid(
        "struct Counter {\n"
        "    value: int\n"
        "    overload as int { return self.value }\n"
        "}\n"
        "counter := Counter()\n"
        "value := counter as int\n");
    expect_valid(
        "class Label {}\n"
        "class Player {\n"
        "    overload as Label { return Label() }\n"
        "}\n"
        "player := Player()\n"
        "label := player as Label\n");
    expect_error(
        "class Broken {\n"
        "    overload as int { return \"wrong\" }\n"
        "}\n",
        "cannot assign value of type 'string' to type 'int'");
    expect_error(
        "class Player { overload as string { return \"player\" } }\n"
        "player := Player()\n"
        "name: string = player\n",
        "cannot assign value of type 'Player' to type 'string'");
    expect_error(
        "class Player {}\n"
        "player := Player()\n"
        "name := player as string\n",
        "no conversion from 'Player' to 'string'");
    expect_error(
        "class Player {}\n"
        "class Team {}\n"
        "player := Player()\n"
        "team := player as Team\n",
        "no conversion from 'Player' to 'Team'");
    expect_error(
        "class Player { overload as string { return \"player\" } }\n"
        "player := Player()\n"
        "value := player as int\n",
        "no conversion from 'Player' to 'int'");
}

void test_construction_and_field_access()
{
    expect_valid(
        "struct Vec2 {\n"
        "    x: dec\n"
        "    y: dec\n"
        "}\n"
        "class Player {\n"
        "    public name: string\n"
        "    public position: Vec2\n"
        "    private health: int = 100\n"
        "    public function damage(amount: int) {\n"
        "        self.health = self.health - amount\n"
        "    }\n"
        "    public function get_health() -> int { return self.health }\n"
        "}\n"
        "function main() {\n"
        "    player := Player(\n"
        "        name: \"Austin\",\n"
        "        position: Vec2(x: 10.0, y: 20.0)\n"
        "    )\n"
        "    player.name = \"toro\"\n"
        "    player.position.x = 15.0\n"
        "    player.damage(amount: 25)\n"
        "    health := player.get_health()\n"
        "    x := player.position.x\n"
        "    print(health)\n"
        "    print(x)\n"
        "}\n");
    expect_valid(
        "struct Point {\n"
        "    x: int\n"
        "    y: int = 0\n"
        "}\n"
        "point := Point(10)\n"
        "point.x = 20\n");
    expect_valid(
        "class User {}\n"
        "struct Defaults {\n"
        "    integer: int\n"
        "    decimal: dec\n"
        "    text: string\n"
        "    flag: bool\n"
        "    owner: User?\n"
        "}\n"
        "defaults := Defaults()\n");

    expect_error(
        "class User {}\n"
        "struct Account {\n"
        "    owner: User\n"
        "}\n"
        "account := Account()\n",
        "missing required field 'owner'");
    expect_error(
        "struct Point { x: int }\n"
        "struct Shape { position: Point }\n"
        "shape := Shape()\n",
        "missing required field 'position'");
    expect_error(
        "struct Point { x: int }\n"
        "point := Point(z: 1)\n",
        "has no field named 'z'");
    expect_error(
        "struct Point { x: int }\n"
        "point := Point(x: \"wrong\")\n",
        "field 'Point.x' expects 'int', got 'string'");
    expect_error(
        "struct Point { x: int }\n"
        "point := Point(1, 2)\n",
        "accepts at most 1 fields, got 2");
    expect_error(
        "struct Point {\n"
        "    x: int\n"
        "    y: int\n"
        "}\n"
        "point := Point(1, x: 2)\n",
        "field 'x' is supplied more than once");
}

void test_member_errors_and_visibility()
{
    expect_error(
        "class Player { private health: int = 100 }\n"
        "player := Player()\n"
        "print(player.health)\n",
        "field 'Player.health' is private");
    expect_error(
        "class Player { private health: int = 100 }\n"
        "player := Player(health: 50)\n",
        "field 'Player.health' is private");
    expect_error(
        "struct Point { x: int }\n"
        "point := Point(x: 1)\n"
        "print(point.y)\n",
        "type 'Point' has no member named 'y'");
    expect_error(
        "struct Point { x: int }\n"
        "point := Point(x: 1)\n"
        "point.x = \"wrong\"\n",
        "cannot assign value of type 'string' to type 'int'");
    expect_error(
        "value := 10\n"
        "value.x = 20\n",
        "cannot access member 'x' on type 'int'");
    expect_error(
        "class Player { public name: string }\n"
        "player := Player(name: \"Austin\")\n"
        "player.name()\n",
        "field 'Player.name' is not callable");
}

void test_method_calls()
{
    expect_valid(
        "class Counter {\n"
        "    private value: int = 0\n"
        "    public function add(amount: int) { self.value = self.value + amount }\n"
        "    public function get() -> int { return self.value }\n"
        "}\n"
        "counter := Counter()\n"
        "counter.add(amount: 2)\n"
        "value: int = counter.get()\n");
    expect_error(
        "class Counter { public function add(amount: int) {} }\n"
        "counter := Counter()\n"
        "counter.add()\n",
        "method 'Counter.add' expects 1 arguments, got 0");
    expect_error(
        "class Counter { public function add(amount: int) {} }\n"
        "counter := Counter()\n"
        "counter.add(\"wrong\")\n",
        "argument 1 to 'Counter.add' expects 'int', got 'string'");
    expect_error(
        "class Counter { public function add(amount: int) {} }\n"
        "counter := Counter()\n"
        "counter.add(value: 1)\n",
        "method 'Counter.add' has no parameter named 'value'");
    expect_error(
        "class Counter { public function reset() {} }\n"
        "counter := Counter()\n"
        "value := counter.reset()\n",
        "expression does not produce a value");
    expect_error(
        "class Counter { function reset() {} }\n"
        "counter := Counter()\n"
        "counter.reset()\n",
        "method 'Counter.reset' is private");
    expect_error(
        "class Counter {}\n"
        "counter := Counter()\n"
        "counter.reset()\n",
        "type 'Counter' has no method named 'reset'");
}

void test_existing_language_features_remain_checkable()
{
    expect_valid(
        "function main() {\n"
        "    message := \"Hello, toro!\"\n"
        "    print(message)\n"
        "}\n");
    expect_valid(
        "function main() {\n"
        "    x := 10\n"
        "    if x > 10 {\n"
        "        print(\"greater\")\n"
        "    } else if x == 10 {\n"
        "        print(\"equal\")\n"
        "    }\n"
        "}\n");
    expect_valid(
        "class Box<T> {\n"
        "    public value: T\n"
        "    function get() -> T { return self.value }\n"
        "}\n"
        "function identity<T>(value: T) -> T { return value }\n"
        "function main() {\n"
        "    box := Box<int>(value: 10)\n"
        "    value := identity<int>(10)\n"
        "    print(box.value)\n"
        "    print(value)\n"
        "}\n");
}

} // namespace

int main()
{
    try {
        test_primitive_inference_and_explicit_types();
        test_assignment_types();
        test_arithmetic_and_comparisons();
        test_logical_operators_and_conditions();
        test_function_arguments();
        test_function_returns();
        test_null_restrictions();
        test_nullable_declarations_and_assignments();
        test_nullable_functions_and_equality();
        test_builtin_casts();
        test_user_defined_conversions();
        test_construction_and_field_access();
        test_member_errors_and_visibility();
        test_method_calls();
        test_existing_language_features_remain_checkable();
    } catch (const std::exception& error) {
        std::cerr << "type checker test failure: " << error.what() << '\n';
        return 1;
    }

    std::cout << "type checker tests passed\n";
    return 0;
}
