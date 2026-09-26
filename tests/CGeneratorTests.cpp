#include "toro/CGenerator.hpp"
#include "toro/Lexer.hpp"
#include "toro/Parser.hpp"
#include "toro/SemanticAnalyzer.hpp"
#include "toro/TypeChecker.hpp"

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

std::string generate(std::string_view source)
{
    auto tokens = toro::Lexer(source).tokenize();
    const auto program = toro::Parser(std::move(tokens)).parse_program();
    toro::SemanticAnalyzer().analyze(program);
    toro::TypeChecker().check(program);
    return toro::CGenerator().generate(program);
}

void expect_contains(
    std::string_view output,
    std::string_view expected,
    std::string_view description)
{
    if (output.find(expected) == std::string_view::npos) {
        throw std::runtime_error(
            "generated C did not contain " + std::string(description)
                + ": " + std::string(expected));
    }
}

void expect_backend_error(std::string_view source, std::string_view expected)
{
    try {
        static_cast<void>(generate(source));
    } catch (const std::runtime_error& error) {
        if (std::string_view(error.what()).find(expected) == std::string_view::npos) {
            throw std::runtime_error(
                "expected backend error containing '" + std::string(expected)
                    + "', got '" + error.what() + "'");
        }
        return;
    }
    throw std::runtime_error("expected C backend failure");
}

void test_primitive_variables_arithmetic_and_assignment()
{
    const auto output = generate(
        "function main() {\n"
        "    count := 10\n"
        "    price := 19.99\n"
        "    active := true\n"
        "    name := \"toro\"\n"
        "    count = count + 2 * 3\n"
        "    print(price)\n"
        "    print(active)\n"
        "    print(name)\n"
        "}\n");

    expect_contains(output, "int64_t toro_var_count = 10;", "integer declaration");
    expect_contains(output, "double toro_var_price = 19.99;", "decimal declaration");
    expect_contains(output, "bool toro_var_active = true;", "boolean declaration");
    expect_contains(output, "const char* toro_var_name = \"toro\";", "string declaration");
    expect_contains(
        output,
        "toro_var_count = (toro_var_count + (2 * 3));",
        "arithmetic reassignment");
    expect_contains(output, "printf(\"%g\\n\", toro_var_price);", "decimal print");
    expect_contains(output, "? \"true\" : \"false\"", "boolean print");
    expect_contains(output, "printf(\"%s\\n\", toro_var_name);", "string print");
}

void test_functions_calls_and_returns()
{
    const auto output = generate(
        "function add(left: int, right: int) -> int {\n"
        "    return left + right\n"
        "}\n"
        "function main() {\n"
        "    result := add(right: 20, left: 10)\n"
        "    print(result)\n"
        "}\n");

    expect_contains(
        output,
        "static int64_t toro_fn_add(int64_t toro_arg_left, int64_t toro_arg_right);",
        "function prototype");
    expect_contains(
        output,
        "return (toro_arg_left + toro_arg_right);",
        "function return");
    expect_contains(
        output,
        "toro_fn_add(10, 20)",
        "deterministically ordered named arguments");
    expect_contains(output, "int main(void)", "C entry point");
}

void test_control_flow_and_boolean_expressions()
{
    const auto output = generate(
        "function main() {\n"
        "    value := 0\n"
        "    running := true\n"
        "    while running and value < 3 {\n"
        "        value = value + 1\n"
        "        if value == 3 or value > 10 {\n"
        "            running = false\n"
        "        } else {\n"
        "            print(value)\n"
        "        }\n"
        "    }\n"
        "}\n");

    expect_contains(
        output,
        "while ((toro_var_running && (toro_var_value < 3)))",
        "while condition");
    expect_contains(
        output,
        "if (((toro_var_value == 3) || (toro_var_value > 10)))",
        "if boolean expression");
    expect_contains(output, "} else {", "else branch");
}

void test_string_equality()
{
    const auto output = generate(
        "function same(left: string, right: string) -> bool {\n"
        "    return left == right\n"
        "}\n");
    expect_contains(output, "strcmp(toro_arg_left, toro_arg_right) == 0", "string equality");
}

void test_struct_definition_construction_and_members()
{
    const auto output = generate(
        "struct Player {\n"
        "    name: string\n"
        "    age: int = 28\n"
        "    active: bool\n"
        "    score: dec\n"
        "}\n"
        "function main() {\n"
        "    player := Player(name: \"Austin\")\n"
        "    player.age = 29\n"
        "    print(player.name)\n"
        "}\n");

    expect_contains(
        output,
        "typedef struct toro_struct_Player toro_struct_Player;",
        "struct forward declaration");
    expect_contains(output, "struct toro_struct_Player", "struct definition");
    expect_contains(output, "const char* toro_field_name;", "string field");
    expect_contains(output, "int64_t toro_field_age;", "integer field");
    expect_contains(
        output,
        "(toro_struct_Player){.toro_field_name = \"Austin\", "
        ".toro_field_age = 28, .toro_field_active = false, "
        ".toro_field_score = 0.0}",
        "explicit construction values and defaults");
    expect_contains(
        output, "(toro_var_player).toro_field_age = 29;", "field assignment");
    expect_contains(
        output,
        "printf(\"%s\\n\", (toro_var_player).toro_field_name)",
        "field access");
}

void test_nested_structs_and_value_copy()
{
    const auto output = generate(
        "struct Point {\n"
        "    x: dec\n"
        "    y: dec\n"
        "}\n"
        "struct Sprite {\n"
        "    position: Point\n"
        "    label: string\n"
        "}\n"
        "function main() {\n"
        "    sprite := Sprite(Point(1.0, 2.0), \"hero\")\n"
        "    copy := sprite\n"
        "    copy.position.x = 3.0\n"
        "    print(sprite.position.x)\n"
        "}\n");

    expect_contains(
        output,
        "toro_struct_Point toro_field_position;",
        "nested struct field");
    expect_contains(
        output,
        "toro_struct_Sprite toro_var_copy = toro_var_sprite;",
        "struct value copy");
    expect_contains(
        output,
        "((toro_var_copy).toro_field_position).toro_field_x = 3.0;",
        "chained field assignment");
}

void test_struct_methods()
{
    const auto output = generate(
        "struct Counter {\n"
        "    value: int\n"
        "    function add(amount: int) { self.value = self.value + amount }\n"
        "    function get() -> int { return self.value }\n"
        "}\n"
        "function main() {\n"
        "    counter := Counter()\n"
        "    counter.add(amount: 4)\n"
        "    print(counter.get())\n"
        "}\n");

    expect_contains(
        output,
        "static void toro_method_7_Counter_add(toro_struct_Counter* toro_self, "
        "int64_t toro_arg_amount);",
        "method prototype with receiver");
    expect_contains(
        output,
        "(toro_self)->toro_field_value = "
        "((toro_self)->toro_field_value + toro_arg_amount);",
        "self mutation");
    expect_contains(
        output,
        "toro_method_7_Counter_add(&(toro_var_counter), 4);",
        "method call receiver");
    expect_contains(
        output,
        "toro_method_7_Counter_get(&(toro_var_counter))",
        "method result call");
}

void test_enum_representation_construction_and_handle()
{
    const auto output = generate(
        "enum Value {\n"
        "    integer(int)\n"
        "    decimal(dec)\n"
        "    flag(bool)\n"
        "    text(string)\n"
        "    empty\n"
        "}\n"
        "function show(value: Value) {\n"
        "    handle value {\n"
        "        integer(item) { print(item) }\n"
        "        decimal(item) { print(item) }\n"
        "        flag(item) { print(item) }\n"
        "        text(item) { print(item) }\n"
        "        empty { print(\"empty\") }\n"
        "    }\n"
        "}\n"
        "function main() {\n"
        "    value := Value::text(\"toro\")\n"
        "    value = Value::empty\n"
        "    show(value)\n"
        "}\n");

    expect_contains(
        output,
        "typedef enum toro_enum_5_Value_tag",
        "enum tag declaration");
    expect_contains(
        output,
        "struct toro_enum_5_Value",
        "tagged enum structure");
    expect_contains(output, "int64_t toro_variant_integer;", "integer payload");
    expect_contains(output, "double toro_variant_decimal;", "decimal payload");
    expect_contains(output, "bool toro_variant_flag;", "boolean payload");
    expect_contains(output, "const char* toro_variant_text;", "string payload");
    expect_contains(
        output,
        "(toro_enum_5_Value){.toro_tag = toro_enum_5_Value_tag_text, "
        ".toro_payload.toro_variant_text = \"toro\"}",
        "payload variant construction");
    expect_contains(
        output,
        "toro_var_value = (toro_enum_5_Value){.toro_tag = "
        "toro_enum_5_Value_tag_empty};",
        "payload-free variant assignment");
    expect_contains(
        output,
        "switch (toro_handle_value_0.toro_tag)",
        "handle switch");
    expect_contains(
        output,
        "const char* toro_var_item = "
        "toro_handle_value_0.toro_payload.toro_variant_text;",
        "scoped payload binding");
}

void test_enum_function_return_and_nested_payload_types()
{
    const auto output = generate(
        "struct Point { x: int }\n"
        "enum Inner { number(int) }\n"
        "enum Outer {\n"
        "    nested(Inner)\n"
        "    point(Point)\n"
        "    done\n"
        "}\n"
        "function make() -> Outer {\n"
        "    return Outer::nested(Inner::number(7))\n"
        "}\n"
        "function main() {\n"
        "    first := make()\n"
        "    copy := first\n"
        "    copy = Outer::point(Point(3))\n"
        "}\n");

    expect_contains(
        output,
        "toro_enum_5_Inner toro_variant_nested;",
        "enum payload type");
    expect_contains(
        output,
        "toro_struct_Point toro_variant_point;",
        "struct payload type");
    expect_contains(
        output,
        "static toro_enum_5_Outer toro_fn_make(void);",
        "enum function return");
    expect_contains(
        output,
        "toro_enum_5_Outer toro_var_copy = toro_var_first;",
        "enum value copy");
}

void test_result_construction_handle_and_propagation()
{
    const auto output = generate(
        "function parse(valid: bool) -> Result<int, string> {\n"
        "    if valid { return ok(7) }\n"
        "    return error(\"bad\")\n"
        "}\n"
        "function forward(valid: bool) -> Result<int, string> {\n"
        "    value := parse(valid)?\n"
        "    return ok(value)\n"
        "}\n"
        "function show(result: Result<int, string>) {\n"
        "    handle result {\n"
        "        ok(value) { print(value) }\n"
        "        error(problem) { print(problem) }\n"
        "    }\n"
        "}\n"
        "function main() {\n"
        "    success: Result<int, string> = forward(true)\n"
        "    copy := success\n"
        "    show(copy)\n"
        "    show(forward(false))\n"
        "}\n");

    expect_contains(
        output,
        "typedef struct toro_result_r1_i_1_s toro_result_r1_i_1_s;",
        "concrete Result declaration");
    expect_contains(output, "toro_result_r1_i_1_s_tag_ok", "Result ok tag");
    expect_contains(output, "int64_t toro_ok;", "Result ok payload");
    expect_contains(output, "const char* toro_error;", "Result error payload");
    expect_contains(
        output,
        ".toro_payload.toro_ok = 7",
        "ok construction");
    expect_contains(
        output,
        ".toro_payload.toro_error = \"bad\"",
        "error construction");
    expect_contains(
        output,
        "if (toro_result_value_0.toro_tag == "
        "toro_result_r1_i_1_s_tag_error)",
        "propagation error check");
    expect_contains(
        output,
        "switch (toro_handle_value_1.toro_tag)",
        "Result handle switch");
    expect_contains(
        output,
        "toro_result_r1_i_1_s toro_var_copy = toro_var_success;",
        "Result value copy");
}

void test_distinct_result_instances()
{
    const auto output = generate(
        "function first() -> Result<int, string> { return ok(1) }\n"
        "function second() -> Result<string, int> { return ok(\"two\") }\n");
    expect_contains(output, "toro_result_r1_i_1_s", "first Result instance");
    expect_contains(output, "toro_result_r1_s_1_i", "second Result instance");
}

void test_class_arc_lowering()
{
    const auto output = generate(
        "class Counter {\n"
        "    public value: int\n"
        "    private step: int = 1\n"
        "    public function increment() { self.value = self.value + self.step }\n"
        "    public function get() -> int { return self.value }\n"
        "}\n"
        "function preserve(value: Counter) -> Counter { return value }\n"
        "function main() {\n"
        "    original := Counter(value: 10)\n"
        "    copy := original\n"
        "    copy.increment()\n"
        "    copy.value = 12\n"
        "    returned := preserve(original)\n"
        "    replacement := Counter(value: 99)\n"
        "    replacement = returned\n"
        "    print(original.get())\n"
        "}\n");

    expect_contains(output, "uint64_t toro_strong_count;", "class ARC count");
    expect_contains(output, "malloc(sizeof(*toro_new_class_", "class allocation");
    expect_contains(output, "->toro_strong_count = 1;", "initial strong reference");
    expect_contains(
        output,
        "static void toro_class_7_Counter_retain",
        "class retain helper");
    expect_contains(
        output,
        "static void toro_class_7_Counter_release",
        "class release helper");
    expect_contains(output, "free(toro_value);", "final strong release frees object");
    expect_contains(
        output,
        "toro_class_7_Counter_retain(toro_var_copy);",
        "reference copy retain");
    expect_contains(
        output,
        "toro_class_7_Counter_release(toro_var_replacement);",
        "replacement releases prior reference");
    expect_contains(
        output,
        "(toro_self)->toro_field_value",
        "class self field access");
}

void test_class_lifecycle_and_weak_lowering()
{
    const auto output = generate(
        "class Child {\n"
        "    public weak parent: Parent?\n"
        "    destroy() { print(\"child destroyed\") }\n"
        "}\n"
        "class Parent {\n"
        "    public child: Child?\n"
        "    destroy() {\n"
        "        print(\"parent destroyed\")\n"
        "        if self.child != null { print(\"child attached\") }\n"
        "    }\n"
        "}\n"
        "class RequiredParent { public child: Child }\n"
        "function main() {\n"
        "    parent := Parent()\n"
        "    child := Child()\n"
        "    parent.child = child\n"
        "    required := RequiredParent(child: child)\n"
        "    child.parent = parent\n"
        "    print(child.parent != null)\n"
        "}\n");

    expect_contains(output, "typedef struct toro_weak_control", "weak control block");
    expect_contains(output, "toro_weak_ref toro_field_parent;", "weak field slot");
    expect_contains(output, "toro_weak_set(", "weak assignment helper");
    expect_contains(output, "toro_weak_load(", "safe weak read helper");
    expect_contains(
        output,
        "toro_class_5_Child_retain(toro_field_class_",
        "strong field retain");
    expect_contains(
        output,
        "toro_class_5_Child_release((toro_value)->toro_field_child);",
        "strong field teardown");

    const auto parent_release = output.find(
        "static void toro_class_6_Parent_finalize(void* toro_raw_value)\n{");
    const auto invalidate = output.find(
        "toro_control->toro_object = NULL;", parent_release);
    const auto destroy = output.find(
        "toro_method_6_Parent_destroy(toro_value);",
        parent_release);
    const auto release_field = output.find(
        "toro_class_5_Child_release((toro_value)->toro_field_child);");
    const auto free_object = output.find("free(toro_value);", release_field);
    if (!(invalidate != std::string::npos && destroy != std::string::npos
            && release_field != std::string::npos
            && free_object != std::string::npos && invalidate < destroy
            && destroy < release_field && release_field < free_object)) {
        throw std::runtime_error(
            "class finalization order was not invalidate, destroy, fields, free");
    }
}

void test_class_inheritance_lowering()
{
    const auto output = generate(
        "class Animal {\n"
        "    public name: string\n"
        "    public function get_name() -> string { return self.name }\n"
        "    destroy() { print(\"animal destroyed\") }\n"
        "}\n"
        "class Dog : Animal {\n"
        "    public age: int\n"
        "    destroy() { print(\"dog destroyed\") }\n"
        "}\n"
        "function accept(animal: Animal) { print(animal.get_name()) }\n"
        "function main() {\n"
        "    dog := Dog(name: \"Rex\", age: 4)\n"
        "    print(dog.name)\n"
        "    print(dog.get_name())\n"
        "    animal: Animal = dog\n"
        "    animal.name = \"Max\"\n"
        "    accept(dog)\n"
        "}\n");

    expect_contains(output, "void (*toro_finalize)(void*);", "dynamic finalizer slot");
    expect_contains(
        output,
        "struct toro_class_3_Dog\n{\n"
        "    toro_class_6_Animal toro_base;\n"
        "    int64_t toro_field_age;",
        "embedded base and derived field layout");
    expect_contains(
        output,
        "toro_method_6_Animal_get_name((&(",
        "inherited non-virtual method call");
    expect_contains(
        output,
        "toro_class_6_Animal* toro_var_animal = (&(",
        "derived-to-base upcast");
    expect_contains(
        output,
        "toro_method_3_Dog_destroy(toro_value);",
        "most-derived destroy");
    expect_contains(
        output,
        "toro_method_6_Animal_destroy((&(toro_value)->toro_base));",
        "base destroy chaining");
    expect_contains(
        output,
        "(toro_value)->toro_base.toro_finalize((toro_value)->toro_base."
        "toro_weak_control->toro_object);",
        "dynamic final release");
}

void test_virtual_dispatch_lowering()
{
    const auto output = generate(
        "abstract class Animal {\n"
        "    public name: string\n"
        "    public virtual function speak() -> string\n"
        "    public virtual function label() -> string { return self.name }\n"
        "    public virtual function score(value: int) -> int { return value }\n"
        "    public function describe() -> string { return self.speak() }\n"
        "}\n"
        "class Dog : Animal {\n"
        "    public override function speak() -> string { return \"dog\" }\n"
        "    public override function score(value: int) -> int { return value + 1 }\n"
        "}\n"
        "class Corgi : Dog {\n"
        "    public override function speak() -> string { return \"corgi\" }\n"
        "}\n"
        "function show(animal: Animal) { print(animal.speak()) }\n"
        "function main() {\n"
        "    dog := Dog(name: \"Rex\")\n"
        "    animal: Animal = dog\n"
        "    print(dog.speak())\n"
        "    print(animal.label())\n"
        "    print(animal.describe())\n"
        "    show(Corgi(name: \"Pip\"))\n"
        "}\n");

    expect_contains(
        output,
        "const toro_vtable_6_Animal* toro_vtable;",
        "hierarchy-root vtable pointer");
    expect_contains(
        output,
        "const char* (*toro_virtual_6_Animal_speak)(void* toro_object);",
        "virtual speak slot");
    expect_contains(
        output,
        "const char* (*toro_virtual_6_Animal_label)(void* toro_object);",
        "second virtual slot");
    expect_contains(
        output,
        "int64_t (*toro_virtual_6_Animal_score)(void* toro_object, int64_t toro_arg_value);",
        "virtual slot with parameter");
    expect_contains(
        output,
        "toro_virtual_thunk_3_Dog_speak",
        "concrete override thunk");
    expect_contains(
        output,
        "toro_virtual_thunk_3_Dog_label",
        "inherited virtual thunk");
    expect_contains(
        output,
        "toro_vtable_instance_5_Corgi",
        "multi-level concrete vtable");
    expect_contains(
        output,
        "->toro_virtual_6_Animal_speak(",
        "virtual call through vtable");
    expect_contains(
        output,
        "toro_method_6_Animal_describe(",
        "non-virtual direct call");
}

void test_class_initializer_lowering()
{
    const auto output = generate(
        "class Child { public value: int }\n"
        "class Parent {\n"
        "    public name: string\n"
        "    public health: int = 100\n"
        "    public child: Child\n"
        "    public weak backup: Child?\n"
        "    init(name: string, child: Child) {\n"
        "        self.name = name\n"
        "        self.child = child\n"
        "        self.backup = child\n"
        "    }\n"
        "}\n"
        "function main() {\n"
        "    child := Child(value: 7)\n"
        "    parent := Parent(child: child, name: \"Austin\")\n"
        "    print(parent.health)\n"
        "}\n");

    expect_contains(
        output,
        "toro_method_6_Parent_init(toro_new_class_",
        "initializer invocation after allocation");
    expect_contains(
        output,
        "->toro_field_health = 100;",
        "field default before initializer invocation");
    expect_contains(
        output,
        "toro_class_5_Child_retain(toro_field_class_",
        "strong field assignment in initializer");
    expect_contains(output, "toro_weak_set(", "weak field assignment in initializer");
}

void test_interface_lowering()
{
    const auto output = generate(
        "interface Named {\n"
        "    function name() -> string\n"
        "    function rename(value: string)\n"
        "}\n"
        "interface Counted { function count() -> int }\n"
        "interface Described { function description() -> string }\n"
        "struct Label implements Named {\n"
        "    value: string\n"
        "    function name() -> string { return self.value }\n"
        "    function rename(value: string) { self.value = value }\n"
        "}\n"
        "class Entity implements Named {\n"
        "    public value: string\n"
        "    public virtual function name() -> string { return self.value }\n"
        "    public function rename(value: string) { self.value = value }\n"
        "    public function description() -> string { return self.value }\n"
        "}\n"
        "class Worker : Entity implements Counted, Described {\n"
        "    public amount: int\n"
        "    public override function name() -> string { return \"worker\" }\n"
        "    public function count() -> int { return self.amount }\n"
        "}\n"
        "function show(value: Named) { print(value.name()) }\n"
        "function return_named(worker: Worker) -> Named { return worker }\n"
        "function main() {\n"
        "    label: Named = Label(value: \"label\")\n"
        "    worker := Worker(value: \"entity\", amount: 4)\n"
        "    named: Named = worker\n"
        "    counted: Counted = worker\n"
        "    described: Described = worker\n"
        "    show(label)\n"
        "    print(named.name())\n"
        "    print(counted.count())\n"
        "    print(described.description())\n"
        "    named = return_named(worker)\n"
        "}\n");

    expect_contains(
        output,
        "struct toro_interface_5_Named",
        "interface value representation");
    expect_contains(
        output,
        "toro_struct_Label toro_struct_value_5_Label;",
        "inline struct interface storage");
    expect_contains(
        output,
        "void (*toro_retain)(void*);",
        "class-backed interface retain callback");
    expect_contains(
        output,
        "toro_interface_vtable_instance_5_Named_6_Worker",
        "inherited interface implementation table");
    expect_contains(
        output,
        "->toro_virtual_6_Entity_name(",
        "interface thunk virtual dispatch");
    expect_contains(
        output,
        "toro_interface_5_Named_retain(&toro_var_named)",
        "interface reassignment ownership");
    expect_contains(
        output,
        ".toro_interface_slot_7_Counted_count",
        "multiple interface dispatch tables");
    expect_contains(
        output,
        "toro_method_6_Entity_description(",
        "inherited non-virtual interface implementation");
}

void test_unsupported_features()
{
    expect_backend_error(
        "function convert(value: int) -> int { return value }\n"
        "function convert(value: string) -> string { return value }\n",
        "function overloads are not supported by the C backend");
    expect_backend_error(
        "struct MaybeOwner { owner: string? }\n",
        "nullable type 'string?' is not supported by the C backend");
    expect_backend_error(
        "struct Named {\n"
        "    name: string\n"
        "    overload as string { return self.name }\n"
        "}\n",
        "conversion overloads are not supported by the C backend");
    expect_backend_error(
        "struct Counter {\n"
        "    value: int\n"
        "    function increment() { self.value = self.value + 1 }\n"
        "}\n"
        "function main() { Counter().increment() }\n",
        "receiver must be an addressable value");
    expect_backend_error(
        "class Resource {}\n"
        "enum Event { resource(Resource) }\n",
        "class-reference enum payloads are not supported by the C backend");
    expect_backend_error(
        "class Resource {}\n"
        "function consume(result: Result<Resource, string>) {}\n",
        "class-reference Result payloads are not supported by the C backend");
    expect_backend_error(
        "class Resource {\n"
        "    destroy() {}\n"
        "    function invoke() { self.destroy() }\n"
        "}\n",
        "destroy() cannot be invoked directly");
    expect_backend_error(
        "interface Box<T> { function value() -> T }\n",
        "generic interfaces are not supported by the C backend");
    expect_backend_error(
        "interface Named { function name() -> string }\n"
        "struct Holder { value: Named }\n",
        "interface-valued struct fields are not supported by the C backend");
    expect_backend_error(
        "class Base { init(value: int) {} }\n"
        "class Child : Base {}\n"
        "function main() { child := Child() }\n",
        "requires unsupported base initializer chaining");
    expect_backend_error(
        "class Named {\n"
        "    public name: string\n"
        "    overload as string { return self.name }\n"
        "}\n",
        "class conversion overloads are not supported by the C backend");
    expect_backend_error(
        "class Node { weak next: Node }\n",
        "weak fields require a nullable class type");
    expect_backend_error(
        "function consume(result: Result<int?, string>) {}\n",
        "nullable type 'int?' is not supported by the C backend");
}

void test_generic_monomorphization()
{
    const auto output = generate(
        "function identity<T>(value: T) -> T { return value }\n"
        "struct Box<T> {\n"
        "    value: T\n"
        "    function echo<U>(value: U) -> U { return value }\n"
        "}\n"
        "class Holder<T> {\n"
        "    public value: T\n"
        "    init(value: T) { self.value = value }\n"
        "    public function echo<U>(value: U) -> U { return value }\n"
        "}\n"
        "function pass_box(value: Box<int>) -> Box<int> { return value }\n"
        "function main() {\n"
        "    number := identity(10)\n"
        "    text := identity<string>(\"Toro\")\n"
        "    boxed := Box(value: 42)\n"
        "    words := Box<string>(value: \"hello\")\n"
        "    nested := Box<Box<int>>(value: pass_box(boxed))\n"
        "    holder := Holder(number)\n"
        "    print(boxed.echo<string>(text))\n"
        "    print(holder.echo<string>(text))\n"
        "    print(words.value)\n"
        "    print(nested.value.value)\n"
        "    print(holder.value)\n"
        "}\n");

    expect_contains(output, "toro_fn_identity__1_i", "int function specialization");
    expect_contains(output, "toro_fn_identity__1_s", "string function specialization");
    expect_contains(output, "toro_struct_Box__1_i", "int struct specialization");
    expect_contains(output, "toro_struct_Box__1_s", "string struct specialization");
    expect_contains(
        output,
        "toro_struct_Box__11_s8_Box__1_i",
        "nested struct specialization");
    expect_contains(output, "toro_class_11_Holder__1_i", "class specialization");
    expect_contains(
        output,
        "toro_method_8_Box__1_i_echo__1_s",
        "generic method specialization");
    expect_contains(
        output,
        "toro_method_11_Holder__1_i_echo__1_s",
        "generic class method specialization");
}

void test_generated_c_compiles()
{
#ifdef TORO_TEST_C_COMPILER
    const auto output = generate(
        "function square(value: int) -> int {\n"
        "    return value * value\n"
        "}\n"
        "function main() {\n"
        "    value := square(4)\n"
        "    if value >= 16 and value != 0 {\n"
        "        print(value)\n"
        "    } else {\n"
        "        print(0)\n"
        "    }\n"
        "}\n");
    const auto path = std::filesystem::temp_directory_path()
        / "toro_c_generator_syntax_test.c";
    {
        std::ofstream file(path);
        if (!file) {
            throw std::runtime_error("could not create temporary generated C file");
        }
        file << output;
    }
    const std::string command = std::string("\"") + TORO_TEST_C_COMPILER
        + "\" -std=c11 -fsyntax-only \"" + path.string() + "\"";
    const int result = std::system(command.c_str());
    std::filesystem::remove(path);
    if (result != 0) {
        throw std::runtime_error("system C compiler rejected generated output");
    }
#endif
}

} // namespace

int main()
{
    try {
        test_primitive_variables_arithmetic_and_assignment();
        test_functions_calls_and_returns();
        test_control_flow_and_boolean_expressions();
        test_string_equality();
        test_struct_definition_construction_and_members();
        test_nested_structs_and_value_copy();
        test_struct_methods();
        test_enum_representation_construction_and_handle();
        test_enum_function_return_and_nested_payload_types();
        test_result_construction_handle_and_propagation();
        test_distinct_result_instances();
        test_class_arc_lowering();
        test_class_lifecycle_and_weak_lowering();
        test_class_inheritance_lowering();
        test_virtual_dispatch_lowering();
        test_class_initializer_lowering();
        test_interface_lowering();
        test_generic_monomorphization();
        test_unsupported_features();
        test_generated_c_compiles();
    } catch (const std::exception& error) {
        std::cerr << "C generator test failure: " << error.what() << '\n';
        return 1;
    }

    std::cout << "C generator tests passed\n";
    return 0;
}
