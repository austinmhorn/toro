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

void test_inheritance_and_subtyping()
{
    expect_valid(
        "class Animal {\n"
        "    public name: string\n"
        "    public virtual function speak() -> string { return \"...\" }\n"
        "}\n"
        "class Dog : Animal {\n"
        "    public override function speak() -> string { return \"woof\" }\n"
        "}\n"
        "function accept(animal: Animal) { print(animal.name) }\n"
        "dog := Dog(name: \"Rex\")\n"
        "animal: Animal = dog\n"
        "accept(dog)\n"
        "sound: string = dog.speak()\n");
    expect_error(
        "class Dog : Missing {}\n",
        "base type 'Missing' of class 'Dog' must name an existing class");
    expect_error(
        "struct Animal {}\n"
        "class Dog : Animal {}\n",
        "must name an existing class");
    expect_error(
        "class First : Second {}\n"
        "class Second : First {}\n",
        "inheritance cycle");
    expect_error(
        "class Base { private secret: int }\n"
        "class Derived : Base {\n"
        "    public function reveal() -> int { return self.secret }\n"
        "}\n",
        "field 'Base.secret' is private");
    expect_error(
        "class Animal {}\n"
        "class Dog : Animal {}\n"
        "animal := Animal()\n"
        "dog: Dog = animal\n",
        "cannot assign value of type 'Animal' to type 'Dog'");
}

void test_virtual_and_override_validation()
{
    expect_valid(
        "class Base { public virtual function value(x: int) -> string { return \"base\" } }\n"
        "class Derived : Base {\n"
        "    public override function value(x: int) -> string { return \"derived\" }\n"
        "}\n");
    expect_error(
        "class Base {}\n"
        "class Derived : Base { override function missing() {} }\n",
        "marked override but no inherited method exists");
    expect_error(
        "class Base { public function value() {} }\n"
        "class Derived : Base { public override function value() {} }\n",
        "cannot override non-virtual method");
    expect_error(
        "class Base { public virtual function value(x: int) -> int { return x } }\n"
        "class Derived : Base {\n"
        "    public override function value(x: dec) -> int { return 1 }\n"
        "}\n",
        "does not match the inherited signature");
    expect_error(
        "class Base { public virtual function value() {} }\n"
        "class Derived : Base { public function value() {} }\n",
        "must use override for an inherited virtual method");
}

void test_abstract_classes()
{
    expect_valid(
        "abstract class Shape { public virtual function area() -> dec }\n"
        "class Circle : Shape {\n"
        "    public override function area() -> dec { return 1.0 }\n"
        "}\n"
        "shape: Shape = Circle()\n");
    expect_error(
        "abstract class Shape { public virtual function area() -> dec }\n"
        "shape := Shape()\n",
        "abstract class 'Shape' cannot be constructed");
    expect_error(
        "abstract class Shape { public virtual function area() -> dec }\n"
        "class Circle : Shape {}\n",
        "does not implement abstract method 'area'");
}

void test_interface_conformance()
{
    expect_valid(
        "interface Printable { function print_value() -> string }\n"
        "interface Resettable { function reset() }\n"
        "class Player implements Printable, Resettable {\n"
        "    public name: string\n"
        "    public function print_value() -> string { return self.name }\n"
        "    public function reset() {}\n"
        "}\n"
        "function emit(value: Printable) { print(value.print_value()) }\n"
        "player := Player()\n"
        "printable: Printable = player\n"
        "emit(player)\n");
    expect_valid(
        "interface Printable { function print_value() -> string }\n"
        "struct Label implements Printable {\n"
        "    value: string\n"
        "    function print_value() -> string { return self.value }\n"
        "}\n"
        "function emit(value: Printable) { print(value.print_value()) }\n"
        "label := Label()\n"
        "printable: Printable = label\n"
        "emit(label)\n");
    expect_error(
        "interface Printable { function print_value() -> string }\n"
        "class Player implements Printable {}\n",
        "does not provide public interface method 'Printable.print_value'");
    expect_error(
        "interface Printable { function print_value() -> string }\n"
        "class Player implements Printable {\n"
        "    public function print_value() -> int { return 1 }\n"
        "}\n",
        "does not match interface 'Printable'");
    expect_error(
        "class NotAnInterface {}\n"
        "class Player implements NotAnInterface {}\n",
        "must name an existing interface");
    expect_error(
        "interface First { function value() -> int }\n"
        "interface Second { function value() -> string }\n"
        "class Both implements First, Second {\n"
        "    public function value() -> int { return 1 }\n"
        "}\n",
        "does not match interface");
}

void test_function_overloads()
{
    expect_valid(
        "function describe(value: int) -> string { return \"int\" }\n"
        "function describe(value: string) -> int { return 1 }\n"
        "text: string = describe(10)\n"
        "number: int = describe(\"hello\")\n");
    expect_valid(
        "function choose(value: int) -> int { return value }\n"
        "function choose(text: string) -> string { return text }\n"
        "function choose(first: int, second: int) -> bool { return true }\n"
        "text: string = choose(text: \"toro\")\n"
        "flag: bool = choose(1, 2)\n");
    expect_error(
        "function choose(value: int) {}\n"
        "function choose(value: string) {}\n"
        "choose(true)\n",
        "no matching overload for 'choose'");
    expect_error(
        "function test(x: int) {}\n"
        "function test(value: int) {}\n",
        "duplicate callable signature for function 'test'");
    expect_error(
        "function test(x: int) -> int { return x }\n"
        "function test(x: int) -> string { return \"x\" }\n",
        "duplicate callable signature for function 'test'");
    expect_error(
        "function inspect(value: dec) {}\n"
        "inspect(10)\n",
        "no matching overload for 'inspect'");
    expect_valid(
        "function inspect(value: int) -> int { return 1 }\n"
        "function inspect(value: dec) -> string { return \"dec\" }\n"
        "value := 10\n"
        "selected: string = inspect(value as dec)\n");
}

void test_method_overloads()
{
    expect_valid(
        "class Formatter {\n"
        "    public function format(value: int) -> string { return \"int\" }\n"
        "    public function format(value: string) -> int { return 1 }\n"
        "    public function format(left: int, right: int) -> bool { return true }\n"
        "}\n"
        "formatter := Formatter()\n"
        "text: string = formatter.format(10)\n"
        "number: int = formatter.format(value: \"hello\")\n"
        "flag: bool = formatter.format(1, 2)\n");
    expect_error(
        "class Formatter {\n"
        "    public function format(value: int) {}\n"
        "    public function format(other: int) -> string { return \"x\" }\n"
        "}\n",
        "duplicate callable signature for method 'Formatter.format'");
    expect_error(
        "class Formatter { public function format(value: int) {} }\n"
        "formatter := Formatter()\n"
        "formatter.format(true)\n",
        "no matching overload for 'Formatter.format'");
}

void test_overload_ranking_and_inheritance()
{
    expect_valid(
        "class Animal {}\n"
        "class Dog : Animal {}\n"
        "function select(value: Animal) -> int { return 1 }\n"
        "function select(value: Dog) -> string { return \"dog\" }\n"
        "selected: string = select(Dog())\n");
    expect_valid(
        "interface Named { function name() -> string }\n"
        "class Player implements Named {\n"
        "    public function name() -> string { return \"player\" }\n"
        "}\n"
        "function select(value: Named) -> int { return 1 }\n"
        "function select(value: Player) -> string { return \"player\" }\n"
        "selected: string = select(Player())\n");
    expect_error(
        "interface First {}\n"
        "interface Second {}\n"
        "class Both implements First, Second {}\n"
        "function select(value: First) {}\n"
        "function select(value: Second) {}\n"
        "select(Both())\n",
        "ambiguous overload for 'select'");
    expect_valid(
        "class Base {\n"
        "    public function convert(value: int) -> string { return \"int\" }\n"
        "    public virtual function convert(value: bool) -> bool { return value }\n"
        "}\n"
        "class Derived : Base {\n"
        "    public function convert(value: string) -> int { return 1 }\n"
        "    public override function convert(value: bool) -> bool { return value }\n"
        "}\n"
        "derived := Derived()\n"
        "text: string = derived.convert(10)\n"
        "number: int = derived.convert(\"value\")\n"
        "flag: bool = derived.convert(true)\n");
}

void test_generic_overload_fallback()
{
    expect_valid(
        "function choose<T>(value: T) -> string { return \"generic\" }\n"
        "function choose(value: int) -> int { return value }\n"
        "selected: int = choose(10)\n"
        "fallback: string = choose<string>(\"hello\")\n");
}

void test_generic_function_inference()
{
    expect_valid(
        "function identity<T>(value: T) -> T { return value }\n"
        "integer: int = identity(10)\n"
        "text: string = identity(\"hi\")\n");
    expect_valid(
        "function choose<T>(a: T, b: T) -> T { return a }\n"
        "integer: int = choose(10, 20)\n"
        "text: string = choose(b: \"second\", a: \"first\")\n");
    expect_valid(
        "function first<A, B>(a: A, b: B) -> A { return a }\n"
        "integer: int = first(10, \"ignored\")\n"
        "text: string = first(\"value\", false)\n");
    expect_valid(
        "function make_list<T>(value: T) -> List<T> {}\n"
        "items: List<int> = make_list(10)\n");
    expect_error(
        "function choose<T>(a: T, b: T) -> T { return a }\n"
        "value := choose(10, \"hello\")\n",
        "conflicting inference for generic parameter 'T'");
}

void test_explicit_generic_arguments()
{
    expect_valid(
        "function identity<T>(value: T) -> T { return value }\n"
        "integer: int = identity<int>(10)\n"
        "text: string = identity<string>(\"hello\")\n");
    expect_error(
        "function identity<T>(value: T) -> T { return value }\n"
        "value := identity<int, string>(10)\n",
        "expected 1 explicit generic arguments, got 2");
    expect_error(
        "function identity<T>(value: T) -> T { return value }\n"
        "value := identity<int>(\"wrong\")\n",
        "conflicting inference for generic parameter 'T'");
    expect_error(
        "function duplicate<T>(value: T) {}\n"
        "function duplicate<U>(other: U) {}\n",
        "duplicate callable signature for function 'duplicate'");
}

void test_generic_constraints()
{
    expect_valid(
        "interface Comparable {}\n"
        "class Number implements Comparable {}\n"
        "function max<T: Comparable>(a: T, b: T) -> T { return a }\n"
        "number: Number = max(Number(), Number())\n");
    expect_valid(
        "interface Serializable {}\n"
        "interface Comparable {}\n"
        "struct Data implements Serializable, Comparable {}\n"
        "function process<T: Serializable + Comparable>(value: T) -> T {\n"
        "    return value\n"
        "}\n"
        "data: Data = process(Data())\n");
    expect_error(
        "interface Comparable {}\n"
        "class Plain {}\n"
        "function max<T: Comparable>(value: T) -> T { return value }\n"
        "value := max(Plain())\n",
        "type 'Plain' does not satisfy constraint 'Comparable'");
    expect_error(
        "interface Serializable {}\n"
        "interface Comparable {}\n"
        "class Partial implements Serializable {}\n"
        "function process<T: Serializable + Comparable>(value: T) -> T {\n"
        "    return value\n"
        "}\n"
        "value := process(Partial())\n",
        "does not satisfy constraint 'Comparable'");
}

void test_generic_overload_resolution()
{
    expect_valid(
        "function inspect<T>(value: T) -> string { return \"generic\" }\n"
        "function inspect(value: int) -> int { return value }\n"
        "concrete: int = inspect(10)\n"
        "fallback: string = inspect(true)\n");
    expect_error(
        "function select<T>(first: T, second: int) {}\n"
        "function select<U>(first: string, second: U) {}\n"
        "select(\"value\", 10)\n",
        "ambiguous overload for 'select'");
}

void test_unconstrained_generic_operations()
{
    expect_error(
        "function add<T>(a: T, b: T) -> T { return a + b }\n",
        "cannot use unconstrained generic type 'T'");
    expect_error(
        "function negate<T>(value: T) -> T { return -value }\n",
        "cannot use unconstrained generic type 'T'");
}

void test_generic_type_construction()
{
    expect_valid(
        "struct Pair<A, B> {\n"
        "    first: A\n"
        "    second: B\n"
        "}\n"
        "pair: Pair<int, string> = Pair<int, string>(\n"
        "    first: 10,\n"
        "    second: \"hello\"\n"
        ")\n"
        "first: int = pair.first\n"
        "second: string = pair.second\n");
    expect_valid(
        "struct Pair<A, B> {\n"
        "    first: A\n"
        "    second: B\n"
        "}\n"
        "pair: Pair<int, string> = Pair(first: 10, second: \"hello\")\n");
    expect_valid(
        "class Box<T> { public value: T }\n"
        "box: Box<int> = Box<int>(value: 10)\n"
        "zeroed: Box<int> = Box<int>()\n");
    expect_error(
        "struct Pair<A, B> {\n"
        "    first: A\n"
        "    second: B\n"
        "}\n"
        "pair := Pair<int>(first: 10, second: \"hello\")\n",
        "type 'Pair' expects 2 generic arguments, got 1");
    expect_error(
        "struct Pair<A, B> {\n"
        "    first: A\n"
        "    second: B\n"
        "}\n"
        "pair := Pair<int, string>(first: \"wrong\", second: \"hello\")\n",
        "conflicting inference for generic parameter 'A'");
    expect_error(
        "struct Phantom<T> { value: int }\n"
        "phantom := Phantom(value: 1)\n",
        "cannot infer generic parameter 'T' while constructing 'Phantom'");
}

void test_generic_member_substitution()
{
    expect_valid(
        "class Box<T> {\n"
        "    public value: T\n"
        "    public function get() -> T { return self.value }\n"
        "    public function replace(value: T) { self.value = value }\n"
        "}\n"
        "box: Box<int> = Box(value: 10)\n"
        "field: int = box.value\n"
        "result: int = box.get()\n"
        "box.replace(20)\n");
    expect_error(
        "class Box<T> {\n"
        "    public value: T\n"
        "    public function replace(value: T) { self.value = value }\n"
        "}\n"
        "box: Box<int> = Box(value: 10)\n"
        "box.replace(\"wrong\")\n",
        "no matching overload for 'Box.replace'");
    expect_valid(
        "class User {}\n"
        "function users() -> List<User> {}\n"
        "class Store<T> {\n"
        "    public values: List<T>\n"
        "    public function get_values() -> List<T> { return self.values }\n"
        "}\n"
        "store: Store<User> = Store(values: users())\n"
        "values: List<User> = store.get_values()\n");
    expect_error(
        "class Box<T> { public value: T }\n"
        "integer: Box<int> = Box(value: 10)\n"
        "text: Box<string> = integer\n",
        "cannot assign value of type 'Box<int>' to type 'Box<string>'");
}

void test_generic_type_constraints()
{
    expect_valid(
        "interface Comparable {}\n"
        "class Item implements Comparable {}\n"
        "class SortedBox<T: Comparable> { public value: T }\n"
        "box: SortedBox<Item> = SortedBox(value: Item())\n");
    expect_valid(
        "interface Comparable {}\n"
        "interface Serializable {}\n"
        "struct Item implements Comparable, Serializable {}\n"
        "struct Container<T: Comparable + Serializable> { value: T }\n"
        "container := Container(value: Item())\n");
    expect_error(
        "interface Comparable {}\n"
        "class Plain {}\n"
        "class SortedBox<T: Comparable> { public value: T }\n"
        "box := SortedBox(value: Plain())\n",
        "type 'Plain' does not satisfy constraint 'Comparable'");
}

void test_recursive_structural_inference()
{
    expect_valid(
        "class User {}\n"
        "function users() -> List<User> {}\n"
        "function first<T>(items: List<T>) -> T {}\n"
        "user: User = first(users())\n");
    expect_valid(
        "class User {}\n"
        "function users_by_name() -> Map<string, User> {}\n"
        "function first_value<T>(items: Map<string, T>) -> T {}\n"
        "user: User = first_value(users_by_name())\n");
    expect_valid(
        "class User {}\n"
        "struct Pair<A, B> {\n"
        "    first: A\n"
        "    second: B\n"
        "}\n"
        "function integers() -> List<int> {}\n"
        "function nested<A, B>(value: Pair<A, List<B>>) -> B {}\n"
        "pair: Pair<User, List<int>> = Pair(\n"
        "    first: User(),\n"
        "    second: integers()\n"
        ")\n"
        "number: int = nested(pair)\n");
    expect_error(
        "function integers() -> List<int> {}\n"
        "function strings() -> List<string> {}\n"
        "function combine<T>(a: List<T>, b: List<T>) -> T {}\n"
        "value := combine(integers(), strings())\n",
        "conflicting inference for generic parameter 'T'");
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
        test_inheritance_and_subtyping();
        test_virtual_and_override_validation();
        test_abstract_classes();
        test_interface_conformance();
        test_function_overloads();
        test_method_overloads();
        test_overload_ranking_and_inheritance();
        test_generic_overload_fallback();
        test_generic_function_inference();
        test_explicit_generic_arguments();
        test_generic_constraints();
        test_generic_overload_resolution();
        test_unconstrained_generic_operations();
        test_generic_type_construction();
        test_generic_member_substitution();
        test_generic_type_constraints();
        test_recursive_structural_inference();
        test_existing_language_features_remain_checkable();
    } catch (const std::exception& error) {
        std::cerr << "type checker test failure: " << error.what() << '\n';
        return 1;
    }

    std::cout << "type checker tests passed\n";
    return 0;
}
