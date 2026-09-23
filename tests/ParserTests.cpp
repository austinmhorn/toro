#include "toro/AST.hpp"
#include "toro/Lexer.hpp"
#include "toro/Parser.hpp"

#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

void expect(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

std::unique_ptr<toro::Expr> parse(std::string_view source)
{
    auto tokens = toro::Lexer(source).tokenize();
    return toro::Parser(std::move(tokens)).parse_expression();
}

toro::Program parse_program(std::string_view source)
{
    auto tokens = toro::Lexer(source).tokenize();
    return toro::Parser(std::move(tokens)).parse_program();
}

void expect_dump(std::string_view source, std::string_view expected)
{
    const auto expression = parse(source);
    expect(toro::dump_expression(*expression) == expected, "unexpected expression tree");
}

void expect_program_dump(std::string_view source, std::string_view expected)
{
    const auto program = parse_program(source);
    expect(toro::dump_program(program) == expected, "unexpected statement tree");
}

void test_literals_and_identifier()
{
    expect_dump("42", "Integer(42)\n");
    expect_dump("19.99", "Decimal(19.99)\n");
    expect_dump("\"hello, toro!\"", "String(hello, toro!)\n");
    expect_dump("true", "Bool(true)\n");
    expect_dump("false", "Bool(false)\n");
    expect_dump("null", "Null\n");
    expect_dump("answer", "Identifier(answer)\n");
}

void test_unary_minus()
{
    expect_dump("-10", "Unary(-)\n  Integer(10)\n");
}

void test_multiplication_before_addition()
{
    expect_dump(
        "10 + 20 * 3",
        "Binary(+)\n"
        "  Integer(10)\n"
        "  Binary(*)\n"
        "    Integer(20)\n"
        "    Integer(3)\n");
}

void test_parentheses_override_precedence()
{
    expect_dump(
        "(10 + 20) * 3",
        "Binary(*)\n"
        "  Grouping\n"
        "    Binary(+)\n"
        "      Integer(10)\n"
        "      Integer(20)\n"
        "  Integer(3)\n");
}

void test_comparison_precedence()
{
    expect_dump(
        "1 + 2 < 3 * 4",
        "Binary(<)\n"
        "  Binary(+)\n"
        "    Integer(1)\n"
        "    Integer(2)\n"
        "  Binary(*)\n"
        "    Integer(3)\n"
        "    Integer(4)\n");
}

void test_equality_precedence()
{
    expect_dump(
        "a >= 10 == true",
        "Binary(==)\n"
        "  Binary(>=)\n"
        "    Identifier(a)\n"
        "    Integer(10)\n"
        "  Bool(true)\n");
}

void test_logical_operators()
{
    expect_dump(
        "active and logged_in",
        "Binary(and)\n"
        "  Identifier(active)\n"
        "  Identifier(logged_in)\n");
    expect_dump(
        "admin or owner",
        "Binary(or)\n"
        "  Identifier(admin)\n"
        "  Identifier(owner)\n");
}

void test_logical_precedence()
{
    expect_dump(
        "a == 1 or b == 2 and c == 3",
        "Binary(or)\n"
        "  Binary(==)\n"
        "    Identifier(a)\n"
        "    Integer(1)\n"
        "  Binary(and)\n"
        "    Binary(==)\n"
        "      Identifier(b)\n"
        "      Integer(2)\n"
        "    Binary(==)\n"
        "      Identifier(c)\n"
        "      Integer(3)\n");
    expect_dump(
        "a > 1 and b <= 2",
        "Binary(and)\n"
        "  Binary(>)\n"
        "    Identifier(a)\n"
        "    Integer(1)\n"
        "  Binary(<=)\n"
        "    Identifier(b)\n"
        "    Integer(2)\n");
}

void test_left_associativity()
{
    expect_dump(
        "10 - 5 - 2",
        "Binary(-)\n"
        "  Binary(-)\n"
        "    Integer(10)\n"
        "    Integer(5)\n"
        "  Integer(2)\n");
}

void test_remaining_binary_operators()
{
    expect_dump("8 / 2", "Binary(/)\n  Integer(8)\n  Integer(2)\n");
    expect_dump("1 != 2", "Binary(!=)\n  Integer(1)\n  Integer(2)\n");
    expect_dump("1 <= 2", "Binary(<=)\n  Integer(1)\n  Integer(2)\n");
    expect_dump("2 > 1", "Binary(>)\n  Integer(2)\n  Integer(1)\n");
}

void test_variable_declarations()
{
    expect_program_dump(
        "x := 10",
        "VariableDeclaration(x)\n"
        "  inferred\n"
        "  Integer(10)\n");
    expect_program_dump(
        "price: dec = 19.99",
        "VariableDeclaration(price)\n"
        "  type: dec\n"
        "  Decimal(19.99)\n");

    const auto program = parse_program("\n  value := 1");
    expect(program.statements.size() == 1, "expected one declaration");
    const auto& declaration = static_cast<const toro::VariableDeclarationStmt&>(
        *program.statements.front());
    expect(declaration.location.line == 2, "declaration line was not retained");
    expect(declaration.location.column == 3, "declaration column was not retained");
}

void test_declaration_and_assignment_are_distinct()
{
    const auto program = parse_program("x := 10\nx = 20");
    expect(program.statements.size() == 2, "expected two statements");
    expect(program.statements[0]->kind == toro::StmtKind::VariableDeclaration,
        "declaration was not represented as a declaration");
    expect(program.statements[1]->kind == toro::StmtKind::Assignment,
        "assignment was not represented as an assignment");
}

void test_assignment_expression()
{
    expect_program_dump(
        "x = x + 1",
        "Assignment(x)\n"
        "  Binary(+)\n"
        "    Identifier(x)\n"
        "    Integer(1)\n");
}

void test_multiple_statements_and_blank_lines()
{
    expect_program_dump(
        "x := 10\n\nprice: dec = 19.99\n\nx = x + 5\n\nprint(x)\n",
        "VariableDeclaration(x)\n"
        "  inferred\n"
        "  Integer(10)\n"
        "\n"
        "VariableDeclaration(price)\n"
        "  type: dec\n"
        "  Decimal(19.99)\n"
        "\n"
        "Assignment(x)\n"
        "  Binary(+)\n"
        "    Identifier(x)\n"
        "    Integer(5)\n"
        "\n"
        "ExpressionStatement\n"
        "  Call\n"
        "    Identifier(print)\n"
        "    Identifier(x)\n");
}

void test_call_expression_statement()
{
    expect_program_dump(
        "add(10, 20)",
        "ExpressionStatement\n"
        "  Call\n"
        "    Identifier(add)\n"
        "    Integer(10)\n"
        "    Integer(20)\n");
}

void test_member_access()
{
    expect_dump(
        "player.name",
        "MemberAccess\n"
        "  Identifier(player)\n"
        "  name\n");
    expect_dump(
        "player.position.x",
        "MemberAccess\n"
        "  MemberAccess\n"
        "    Identifier(player)\n"
        "    position\n"
        "  x\n");
    expect_dump(
        "service.client.connect()",
        "Call\n"
        "  MemberAccess\n"
        "    MemberAccess\n"
        "      Identifier(service)\n"
        "      client\n"
        "    connect\n");
}

void test_member_assignment()
{
    expect_program_dump(
        "player.health = 50\n"
        "player.position.x = 10.0\n",
        "MemberAssignment\n"
        "  MemberAccess\n"
        "    Identifier(player)\n"
        "    health\n"
        "  Integer(50)\n"
        "\n"
        "MemberAssignment\n"
        "  MemberAccess\n"
        "    MemberAccess\n"
        "      Identifier(player)\n"
        "      position\n"
        "    x\n"
        "  Decimal(10.0)\n");

    const auto program = parse_program("\n  player.health = 75\n");
    expect(program.statements.front()->kind == toro::StmtKind::MemberAssignment,
        "member assignment did not use its dedicated statement node");
    expect(program.statements.front()->location.line == 2
            && program.statements.front()->location.column == 3,
        "member assignment source location was not retained");
}

void test_named_call_arguments()
{
    expect_dump(
        "create(10, name: \"Austin\", active: true)",
        "Call\n"
        "  Identifier(create)\n"
        "  Integer(10)\n"
        "  NamedArgument(name)\n"
        "    String(Austin)\n"
        "  NamedArgument(active)\n"
        "    Bool(true)\n");
}

void test_nested_multiline_construction()
{
    expect_program_dump(
        "player := Player(\n"
        "    name: \"Austin\",\n"
        "    position: Vec2(\n"
        "        x: 10.0,\n"
        "        y: 20.0\n"
        "    )\n"
        ")\n",
        "VariableDeclaration(player)\n"
        "  inferred\n"
        "  Call\n"
        "    Identifier(Player)\n"
        "    NamedArgument(name)\n"
        "      String(Austin)\n"
        "    NamedArgument(position)\n"
        "      Call\n"
        "        Identifier(Vec2)\n"
        "        NamedArgument(x)\n"
        "          Decimal(10.0)\n"
        "        NamedArgument(y)\n"
        "          Decimal(20.0)\n");
}

void test_empty_function()
{
    expect_program_dump(
        "function main() {\n}\n",
        "FunctionDeclaration(main)\n"
        "  Parameters\n"
        "  Block\n");
}

void test_function_parameters_and_return_type()
{
    expect_program_dump(
        "function add(a: int, b: int) -> int {\n"
        "    return a + b\n"
        "}\n",
        "FunctionDeclaration(add)\n"
        "  Parameters\n"
        "    Parameter(a: int)\n"
        "    Parameter(b: int)\n"
        "  return type: int\n"
        "  Block\n"
        "    Return\n"
        "      Binary(+)\n"
        "        Identifier(a)\n"
        "        Identifier(b)\n");

    const auto program = parse_program("function one(value: dec) {}\n");
    const auto& function = static_cast<const toro::FunctionDeclarationStmt&>(
        *program.statements.front());
    expect(function.location.line == 1 && function.location.column == 1,
        "function location was not retained");
    expect(function.parameters.front().location.column == 14,
        "parameter location was not retained");
}

void test_return_without_value()
{
    expect_program_dump(
        "function finish() {\n"
        "    return\n"
        "}\n",
        "FunctionDeclaration(finish)\n"
        "  Parameters\n"
        "  Block\n"
        "    Return\n");
}

void test_nested_function_statements()
{
    expect_program_dump(
        "function main() {\n"
        "    result := add(10, 20)\n"
        "    print(result)\n"
        "}\n",
        "FunctionDeclaration(main)\n"
        "  Parameters\n"
        "  Block\n"
        "    VariableDeclaration(result)\n"
        "      inferred\n"
        "      Call\n"
        "        Identifier(add)\n"
        "        Integer(10)\n"
        "        Integer(20)\n"
        "    ExpressionStatement\n"
        "      Call\n"
        "        Identifier(print)\n"
        "        Identifier(result)\n");
}

void test_multiple_functions()
{
    const auto program = parse_program(
        "function first() {\n}\n\n"
        "function second(value: int) -> int {\n"
        "    return value\n"
        "}\n");
    expect(program.statements.size() == 2, "expected two function declarations");
    expect(program.statements[0]->kind == toro::StmtKind::FunctionDeclaration,
        "first statement was not a function");
    expect(program.statements[1]->kind == toro::StmtKind::FunctionDeclaration,
        "second statement was not a function");
}

void test_standalone_block()
{
    expect_program_dump(
        "{\n"
        "    x := 10\n"
        "    print(x)\n"
        "}\n",
        "Block\n"
        "  VariableDeclaration(x)\n"
        "    inferred\n"
        "    Integer(10)\n"
        "  ExpressionStatement\n"
        "    Call\n"
        "      Identifier(print)\n"
        "      Identifier(x)\n");
}

void test_basic_if()
{
    expect_program_dump(
        "if x > 5 {\n"
        "    print(\"large\")\n"
        "}\n",
        "If\n"
        "  Condition\n"
        "    Binary(>)\n"
        "      Identifier(x)\n"
        "      Integer(5)\n"
        "  Then\n"
        "    Block\n"
        "      ExpressionStatement\n"
        "        Call\n"
        "          Identifier(print)\n"
        "          String(large)\n");

    const auto program = parse_program("\n  if true {\n  }\n");
    const auto& if_statement = static_cast<const toro::IfStmt&>(
        *program.statements.front());
    expect(if_statement.location.line == 2 && if_statement.location.column == 3,
        "if source location was not retained");
}

void test_if_else()
{
    expect_program_dump(
        "if x > 5 {\n"
        "    print(\"large\")\n"
        "} else {\n"
        "    print(\"small\")\n"
        "}\n",
        "If\n"
        "  Condition\n"
        "    Binary(>)\n"
        "      Identifier(x)\n"
        "      Integer(5)\n"
        "  Then\n"
        "    Block\n"
        "      ExpressionStatement\n"
        "        Call\n"
        "          Identifier(print)\n"
        "          String(large)\n"
        "  Else\n"
        "    Block\n"
        "      ExpressionStatement\n"
        "        Call\n"
        "          Identifier(print)\n"
        "          String(small)\n");
}

void test_else_if_chains()
{
    const auto program = parse_program(
        "if first {\n}\n"
        "else if second {\n}\n"
        "else if third {\n}\n"
        "else {\n}\n");
    const auto& first = static_cast<const toro::IfStmt&>(*program.statements.front());
    expect(first.else_branch->kind == toro::StmtKind::If,
        "first else-if branch was not an if statement");
    const auto& second = static_cast<const toro::IfStmt&>(*first.else_branch);
    expect(second.else_branch->kind == toro::StmtKind::If,
        "second else-if branch was not an if statement");
    const auto& third = static_cast<const toro::IfStmt&>(*second.else_branch);
    expect(third.else_branch->kind == toro::StmtKind::Block,
        "final else branch was not a block");
}

void test_nested_if_and_return()
{
    const auto program = parse_program(
        "function check(x: int, y: int) -> int {\n"
        "    if x > 0 {\n"
        "        if y > 0 {\n"
        "            return x\n"
        "        }\n"
        "    }\n"
        "    return 0\n"
        "}\n");
    const auto& function = static_cast<const toro::FunctionDeclarationStmt&>(
        *program.statements.front());
    const auto& outer_if = static_cast<const toro::IfStmt&>(*function.body->statements.front());
    expect(outer_if.then_block->statements.front()->kind == toro::StmtKind::If,
        "nested if was not retained in the outer block");
    const auto& inner_if = static_cast<const toro::IfStmt&>(
        *outer_if.then_block->statements.front());
    expect(inner_if.then_block->statements.front()->kind == toro::StmtKind::Return,
        "return was not retained inside the nested if");
}

void test_declarations_and_assignments_in_branches()
{
    const auto program = parse_program(
        "if ready {\n"
        "    value := 1\n"
        "} else {\n"
        "    value = 2\n"
        "}\n");
    const auto& if_statement = static_cast<const toro::IfStmt&>(
        *program.statements.front());
    expect(if_statement.then_block->statements.front()->kind
            == toro::StmtKind::VariableDeclaration,
        "declaration was not retained in then branch");
    const auto& else_block = static_cast<const toro::BlockStmt&>(
        *if_statement.else_branch);
    expect(else_block.statements.front()->kind == toro::StmtKind::Assignment,
        "assignment was not retained in else branch");
}

void test_logical_operators_in_if_condition()
{
    const auto program = parse_program(
        "if active and logged_in or admin {\n"
        "    print(\"allowed\")\n"
        "}\n");
    const auto& if_statement = static_cast<const toro::IfStmt&>(
        *program.statements.front());
    expect(
        toro::dump_expression(*if_statement.condition)
            == "Binary(or)\n"
               "  Binary(and)\n"
               "    Identifier(active)\n"
               "    Identifier(logged_in)\n"
               "  Identifier(admin)\n",
        "logical operators parsed incorrectly in if condition");
}

void test_basic_while()
{
    expect_program_dump(
        "while running {\n"
        "    update()\n"
        "}\n",
        "While\n"
        "  Condition\n"
        "    Identifier(running)\n"
        "  Block\n"
        "    ExpressionStatement\n"
        "      Call\n"
        "        Identifier(update)\n");

    const auto program = parse_program("\n  while active {\n    stop\n  }\n");
    const auto& loop = static_cast<const toro::WhileStmt&>(*program.statements.front());
    expect(loop.location.line == 2 && loop.location.column == 3,
        "while source location was not retained");
}

void test_while_logical_condition()
{
    expect_program_dump(
        "while running and ready or forced {\n"
        "    stop\n"
        "}\n",
        "While\n"
        "  Condition\n"
        "    Binary(or)\n"
        "      Binary(and)\n"
        "        Identifier(running)\n"
        "        Identifier(ready)\n"
        "      Identifier(forced)\n"
        "  Block\n"
        "    Stop\n");
}

void test_basic_for_in()
{
    expect_program_dump(
        "for user in users {\n"
        "    print(user)\n"
        "}\n",
        "ForIn(user)\n"
        "  Collection\n"
        "    Identifier(users)\n"
        "  Block\n"
        "    ExpressionStatement\n"
        "      Call\n"
        "        Identifier(print)\n"
        "        Identifier(user)\n");
}

void test_nested_loops_and_loop_control()
{
    const auto program = parse_program(
        "while outer {\n"
        "    for item in items {\n"
        "        stop\n"
        "    }\n"
        "    continue\n"
        "}\n");
    const auto& outer = static_cast<const toro::WhileStmt&>(*program.statements.front());
    expect(outer.body->statements.front()->kind == toro::StmtKind::ForIn,
        "nested for loop was not retained");
    const auto& inner = static_cast<const toro::ForInStmt&>(
        *outer.body->statements.front());
    expect(inner.body->statements.front()->kind == toro::StmtKind::Stop,
        "stop was not retained in nested loop");
    expect(outer.body->statements.back()->kind == toro::StmtKind::Continue,
        "continue was not retained in outer loop");
}

void test_loop_control_inside_conditionals()
{
    const auto program = parse_program(
        "for user in users {\n"
        "    if disabled {\n"
        "        continue\n"
        "    }\n"
        "    if admin {\n"
        "        stop\n"
        "    }\n"
        "}\n");
    const auto& loop = static_cast<const toro::ForInStmt&>(*program.statements.front());
    const auto& continue_if = static_cast<const toro::IfStmt&>(*loop.body->statements[0]);
    const auto& stop_if = static_cast<const toro::IfStmt&>(*loop.body->statements[1]);
    expect(continue_if.then_block->statements.front()->kind == toro::StmtKind::Continue,
        "continue was not retained inside conditional");
    expect(stop_if.then_block->statements.front()->kind == toro::StmtKind::Stop,
        "stop was not retained inside conditional");
}

void test_simple_enum()
{
    expect_program_dump(
        "enum Direction {\n"
        "    north\n"
        "    south\n"
        "    east\n"
        "    west\n"
        "}\n",
        "EnumDeclaration(Direction)\n"
        "  Variant(north)\n"
        "  Variant(south)\n"
        "  Variant(east)\n"
        "  Variant(west)\n");

    const auto program = parse_program("\n  enum State {\n    ready\n  }\n");
    const auto& declaration = static_cast<const toro::EnumDeclarationStmt&>(
        *program.statements.front());
    expect(declaration.location.line == 2 && declaration.location.column == 3,
        "enum source location was not retained");
    expect(declaration.variants.front().location.line == 3,
        "enum variant source location was not retained");
}

void test_enum_payload_variants()
{
    expect_program_dump(
        "enum Message {\n"
        "    text(string)\n"
        "    image(Image)\n"
        "    quit\n"
        "}\n",
        "EnumDeclaration(Message)\n"
        "  Variant(text)\n"
        "    PayloadType(string)\n"
        "  Variant(image)\n"
        "    PayloadType(Image)\n"
        "  Variant(quit)\n");

    const auto program = parse_program("enum Value {\n number(int)\n none\n}\n");
    const auto& declaration = static_cast<const toro::EnumDeclarationStmt&>(
        *program.statements.front());
    expect(declaration.variants.size() == 2, "mixed enum variants were not retained");
    expect(declaration.variants.front().payload_type == "int",
        "enum payload type was not retained");
    expect(!declaration.variants.back().payload_type,
        "payload-free enum variant unexpectedly has a payload");
}

void test_handle_cases()
{
    expect_program_dump(
        "handle message {\n"
        "    text(value) {\n"
        "        print(value)\n"
        "    }\n"
        "\n"
        "    quit {\n"
        "        return\n"
        "    }\n"
        "}\n",
        "Handle\n"
        "  Expression\n"
        "    Identifier(message)\n"
        "  Case(text)\n"
        "    Binding(value)\n"
        "    Block\n"
        "      ExpressionStatement\n"
        "        Call\n"
        "          Identifier(print)\n"
        "          Identifier(value)\n"
        "  Case(quit)\n"
        "    Block\n"
        "      Return\n");

    const auto program = parse_program("handle value {\n some(item) {}\n none {}\n}\n");
    const auto& handle = static_cast<const toro::HandleStmt&>(*program.statements.front());
    expect(handle.location.line == 1 && handle.location.column == 1,
        "handle source location was not retained");
    expect(handle.cases.size() == 2, "multiple handle cases were not retained");
    expect(handle.cases.front().binding_name == "item",
        "handle binding name was not retained");
    expect(!handle.cases.back().binding_name,
        "payload-free handle case unexpectedly has a binding");
    expect(handle.cases.front().location.line == 2,
        "handle case source location was not retained");
}

void test_nested_handle_and_function_context()
{
    const auto program = parse_program(
        "function process(message: Message) {\n"
        "    handle message {\n"
        "        wrapped(value) {\n"
        "            handle value {\n"
        "                text(contents) {\n"
        "                    print(contents)\n"
        "                }\n"
        "            }\n"
        "        }\n"
        "    }\n"
        "}\n");
    const auto& function = static_cast<const toro::FunctionDeclarationStmt&>(
        *program.statements.front());
    const auto& outer = static_cast<const toro::HandleStmt&>(
        *function.body->statements.front());
    expect(outer.cases.front().body->statements.front()->kind == toro::StmtKind::Handle,
        "nested handle was not retained in its case body");
}

void test_handle_preserves_loop_context()
{
    const auto program = parse_program(
        "while running {\n"
        "    handle message {\n"
        "        quit {\n"
        "            stop\n"
        "        }\n"
        "    }\n"
        "}\n");
    const auto& loop = static_cast<const toro::WhileStmt&>(*program.statements.front());
    const auto& handle = static_cast<const toro::HandleStmt&>(*loop.body->statements.front());
    expect(handle.cases.front().body->statements.front()->kind == toro::StmtKind::Stop,
        "handle case body did not preserve loop context");
}

void test_struct_declarations()
{
    expect_program_dump(
        "struct Empty {\n}\n\n"
        "struct Vec2 {\n"
        "    x: dec\n"
        "    y: dec\n"
        "}\n",
        "StructDeclaration(Empty)\n"
        "\n"
        "StructDeclaration(Vec2)\n"
        "  Field(x: dec)\n"
        "  Field(y: dec)\n");

    const auto program = parse_program("\n  struct Point {\n    x: dec\n  }\n");
    const auto& declaration = static_cast<const toro::StructDeclarationStmt&>(
        *program.statements.front());
    expect(declaration.location.line == 2 && declaration.location.column == 3,
        "struct source location was not retained");
    expect(declaration.fields.front().location.line == 3,
        "struct field source location was not retained");
}

void test_struct_field_defaults()
{
    expect_program_dump(
        "struct Player {\n"
        "    name: string = \"\"\n"
        "    health: int = 100\n"
        "    position: Vec2\n"
        "}\n",
        "StructDeclaration(Player)\n"
        "  Field(name: string)\n"
        "    Default\n"
        "      String()\n"
        "  Field(health: int)\n"
        "    Default\n"
        "      Integer(100)\n"
        "  Field(position: Vec2)\n");

    const auto program = parse_program("struct Config {\n enabled: bool = true\n}\n");
    const auto& declaration = static_cast<const toro::StructDeclarationStmt&>(
        *program.statements.front());
    expect(declaration.fields.front().default_value != nullptr,
        "struct field default expression was not retained");
}

void test_empty_class()
{
    expect_program_dump(
        "class Empty {\n}\n",
        "ClassDeclaration(Empty)\n");

    const auto program = parse_program("\n  class Empty {\n  }\n");
    const auto& declaration = static_cast<const toro::ClassDeclarationStmt&>(
        *program.statements.front());
    expect(declaration.location.line == 2 && declaration.location.column == 3,
        "class source location was not retained");
    expect(declaration.members.empty(), "empty class unexpectedly has members");
}

void test_class_fields_and_visibility()
{
    expect_program_dump(
        "class Player {\n"
        "    public name: string\n"
        "    private health: int = 100\n"
        "    score: int\n"
        "}\n",
        "ClassDeclaration(Player)\n"
        "  public Field(name: string)\n"
        "  private Field(health: int)\n"
        "    Default\n"
        "      Integer(100)\n"
        "  private Field(score: int)\n");

    const auto program = parse_program(
        "class Player {\n public name: string\n health: int\n}\n");
    const auto& declaration = static_cast<const toro::ClassDeclarationStmt&>(
        *program.statements.front());
    expect(declaration.members.size() == 2, "class member order was not retained");
    expect(declaration.members[0]->visibility == toro::Visibility::Public,
        "public field visibility was not retained");
    expect(declaration.members[1]->visibility == toro::Visibility::Private,
        "default class visibility was not private");
    expect(declaration.members[0]->location.line == 2,
        "class member source location was not retained");
}

void test_class_methods_init_and_destroy()
{
    expect_program_dump(
        "class Player {\n"
        "    function init(name: string) {\n"
        "        self.name = name\n"
        "    }\n"
        "\n"
        "    public function get_health() -> int {\n"
        "        return self.health\n"
        "    }\n"
        "\n"
        "    function destroy() {\n"
        "        print(\"player destroyed\")\n"
        "    }\n"
        "}\n",
        "ClassDeclaration(Player)\n"
        "  private Method(init)\n"
        "    Parameters\n"
        "      Parameter(name: string)\n"
        "    Block\n"
        "      MemberAssignment\n"
        "        MemberAccess\n"
        "          Identifier(self)\n"
        "          name\n"
        "        Identifier(name)\n"
        "  public Method(get_health)\n"
        "    Parameters\n"
        "    return type: int\n"
        "    Block\n"
        "      Return\n"
        "        MemberAccess\n"
        "          Identifier(self)\n"
        "          health\n"
        "  private Method(destroy)\n"
        "    Parameters\n"
        "    Block\n"
        "      ExpressionStatement\n"
        "        Call\n"
        "          Identifier(print)\n"
        "          String(player destroyed)\n");

    const auto program = parse_program(
        "class Worker {\n public function run(value: int) -> int { return value }\n}\n");
    const auto& declaration = static_cast<const toro::ClassDeclarationStmt&>(
        *program.statements.front());
    const auto& method = static_cast<const toro::MethodDeclaration&>(
        *declaration.members.front());
    expect(method.parameters.size() == 1 && method.return_type == "int",
        "method parameters or return type were not retained");
    expect(method.location.line == 2, "method source location was not retained");
}

void test_self_member_and_method_calls()
{
    expect_dump(
        "self.health",
        "MemberAccess\n"
        "  Identifier(self)\n"
        "  health\n");
    expect_program_dump(
        "player.damage(25)\n"
        "print(player.get_health())\n",
        "ExpressionStatement\n"
        "  Call\n"
        "    MemberAccess\n"
        "      Identifier(player)\n"
        "      damage\n"
        "    Integer(25)\n"
        "\n"
        "ExpressionStatement\n"
        "  Call\n"
        "    Identifier(print)\n"
        "    Call\n"
        "      MemberAccess\n"
        "        Identifier(player)\n"
        "        get_health\n");
}

void expect_parse_error(std::string_view source, std::string_view expected_message)
{
    try {
        static_cast<void>(parse(source));
    } catch (const std::runtime_error& error) {
        expect(std::string_view(error.what()).find(expected_message) != std::string_view::npos,
            "parse error message was not descriptive");
        return;
    }
    throw std::runtime_error("expected parser failure");
}

void test_failures()
{
    expect_parse_error("10 +", "line 1, column 5: expected expression");
    expect_parse_error("(10 + 20", "line 1, column 9: expected ')' after expression");
    expect_parse_error("* 10", "line 1, column 1: expected expression");
    expect_parse_error("10 20", "line 1, column 4: unexpected token after expression");
}

void expect_program_error(std::string_view source, std::string_view expected_message)
{
    try {
        static_cast<void>(parse_program(source));
    } catch (const std::runtime_error& error) {
        if (std::string_view(error.what()).find(expected_message) == std::string_view::npos) {
            throw std::runtime_error(
                "expected statement error containing '" + std::string(expected_message)
                + "', got '" + error.what() + "'");
        }
        return;
    }
    throw std::runtime_error("expected statement parser failure");
}

void test_statement_failures()
{
    expect_program_error("x: = 10", "line 1, column 4: expected type name after ':'");
    expect_program_error("10 = x", "line 1, column 4: invalid assignment target");
    expect_program_error("x :=", "line 1, column 5: expected variable initializer");
    expect_program_error("x: int", "line 1, column 7: expected '=' and initializer");
    expect_program_error("x := 10\n* 2", "line 2, column 1: expected expression");
    expect_program_error(":= 10", "line 1, column 1: expected expression");
}

void test_function_failures()
{
    expect_program_error(
        "function bad(a: int,) {}",
        "expected parameter name");
    expect_program_error(
        "function bad(a) {}",
        "expected ':' after parameter name");
    expect_program_error(
        "function bad(a:) {}",
        "expected parameter type after ':'");
    expect_program_error(
        "function bad() {\n    return\n",
        "expected '}' after block");
    expect_program_error(
        "function bad() {\n    return + 1\n}\n",
        "expected expression");
}

void test_if_failures()
{
    expect_program_error(
        "if {\n}\n",
        "expected condition after 'if'");
    expect_program_error(
        "if x > 5\n    print(x)\n",
        "expected '{' after if condition");
    expect_program_error(
        "if x > 5 {\n    print(x)\n",
        "expected '}' after block");
    expect_program_error(
        "else {\n}\n",
        "unexpected 'else' without matching 'if'");
    expect_program_error(
        "if first {\n}\nelse if second\n    print(second)\n",
        "expected '{' after if condition");
}

void test_loop_failures()
{
    expect_program_error(
        "while {\n}\n",
        "expected condition after 'while'");
    expect_program_error(
        "while running\n    update()\n",
        "expected '{' after while condition");
    expect_program_error(
        "while running {\n    update()\n",
        "expected '}' after block");
    expect_program_error(
        "for in users {\n}\n",
        "expected loop variable after 'for'");
    expect_program_error(
        "for user users {\n}\n",
        "expected 'in' after loop variable");
    expect_program_error(
        "for user in {\n}\n",
        "expected collection expression after 'in'");
    expect_program_error(
        "for user in users\n    print(user)\n",
        "expected '{' after for collection");
    expect_program_error(
        "stop\n",
        "'stop' is only valid inside a loop");
    expect_program_error(
        "continue\n",
        "'continue' is only valid inside a loop");
    expect_program_error(
        "while running {\n"
        "    function nested() {\n"
        "        stop\n"
        "    }\n"
        "}\n",
        "'stop' is only valid inside a loop");
}

void test_enum_failures()
{
    expect_program_error("enum {\n value\n}\n", "expected enum name");
    expect_program_error("enum Empty {\n}\n", "enum must declare at least one variant");
    expect_program_error(
        "enum Message {\n text()\n}\n",
        "expected payload type after '('");
    expect_program_error(
        "enum Message {\n text(string\n}\n",
        "expected ')' after payload type");
    expect_program_error(
        "enum Message {\n text(string) quit\n}\n",
        "unexpected token after statement");
    expect_program_error(
        "enum Message {\n text(string)\n",
        "expected '}' after enum body");
}

void test_handle_failures()
{
    expect_program_error("handle {\n case {}\n}\n", "expected expression after 'handle'");
    expect_program_error(
        "handle message\n text(value) {}\n",
        "expected '{' after handle expression");
    expect_program_error("handle message {\n}\n", "handle must declare at least one case");
    expect_program_error(
        "handle message {\n return {}\n}\n",
        "expected handle case variant name");
    expect_program_error(
        "handle message {\n text() {}\n}\n",
        "expected binding name after '('");
    expect_program_error(
        "handle message {\n text(value {\n}\n}\n",
        "expected ')' after binding name");
    expect_program_error(
        "handle message {\n text(value)\n}\n",
        "expected '{' before handle case body");
    expect_program_error(
        "handle message {\n text(value) {\n print(value)\n}\n",
        "expected '}' after handle cases");
    expect_program_error(
        "handle message {\n quit {\n stop\n }\n}\n",
        "'stop' is only valid inside a loop");
}

void test_struct_and_member_failures()
{
    expect_program_error("struct {\n}\n", "expected struct name");
    expect_program_error(
        "struct Broken {\n value int\n}\n",
        "expected ':' after struct field name");
    expect_program_error(
        "struct Broken {\n value:\n}\n",
        "expected struct field type after ':'");
    expect_program_error(
        "struct Broken {\n value: int =\n}\n",
        "expected default value after '='");
    expect_program_error(
        "struct Broken {\n value: int,\n}\n",
        "unexpected token after statement");
    expect_program_error(
        "struct Broken {\n value: int\n",
        "expected '}' after struct body");
    expect_program_error("player. = 10", "expected member name after '.'");
    expect_program_error("(a + b) = 5", "invalid assignment target");
}

void test_named_argument_failures()
{
    expect_program_error(
        "create(name: \"Austin\", 10)",
        "positional argument cannot follow named argument");
    expect_program_error(
        "create(name: \"Austin\", name: \"other\")",
        "duplicate named argument");
    expect_program_error("create(name:)", "expected value after named argument");
}

void test_class_failures()
{
    expect_program_error("class {\n}\n", "expected class name");
    expect_program_error("class Broken\n", "expected '{' before class body");
    expect_program_error(
        "class Broken {\n public\n}\n",
        "expected class field or method");
    expect_program_error(
        "class Broken {\n value int\n}\n",
        "expected ':' after class field name");
    expect_program_error(
        "class Broken {\n value:\n}\n",
        "expected class field type after ':'");
    expect_program_error(
        "class Broken {\n function run(value) {}\n}\n",
        "expected ':' after parameter name");
    expect_program_error(
        "class Broken {\n function run()\n}\n",
        "expected '{' before method body");
    expect_program_error(
        "class Broken {\n value: int\n",
        "expected '}' after class body");
}

void test_destroy_failures()
{
    expect_program_error(
        "class Broken {\n function destroy(reason: string) {}\n}\n",
        "destroy method cannot declare parameters");
    expect_program_error(
        "class Broken {\n function destroy() -> int {}\n}\n",
        "destroy method cannot declare a return type");
    expect_program_error(
        "class Broken {\n function destroy() {}\n function destroy() {}\n}\n",
        "class may declare at most one destroy method");
}

} // namespace

int main()
{
    try {
        test_literals_and_identifier();
        test_unary_minus();
        test_multiplication_before_addition();
        test_parentheses_override_precedence();
        test_comparison_precedence();
        test_equality_precedence();
        test_logical_operators();
        test_logical_precedence();
        test_left_associativity();
        test_remaining_binary_operators();
        test_variable_declarations();
        test_declaration_and_assignment_are_distinct();
        test_assignment_expression();
        test_multiple_statements_and_blank_lines();
        test_call_expression_statement();
        test_member_access();
        test_member_assignment();
        test_named_call_arguments();
        test_nested_multiline_construction();
        test_empty_function();
        test_function_parameters_and_return_type();
        test_return_without_value();
        test_nested_function_statements();
        test_multiple_functions();
        test_standalone_block();
        test_basic_if();
        test_if_else();
        test_else_if_chains();
        test_nested_if_and_return();
        test_declarations_and_assignments_in_branches();
        test_logical_operators_in_if_condition();
        test_basic_while();
        test_while_logical_condition();
        test_basic_for_in();
        test_nested_loops_and_loop_control();
        test_loop_control_inside_conditionals();
        test_simple_enum();
        test_enum_payload_variants();
        test_handle_cases();
        test_nested_handle_and_function_context();
        test_handle_preserves_loop_context();
        test_struct_declarations();
        test_struct_field_defaults();
        test_empty_class();
        test_class_fields_and_visibility();
        test_class_methods_init_and_destroy();
        test_self_member_and_method_calls();
        test_failures();
        test_statement_failures();
        test_function_failures();
        test_if_failures();
        test_loop_failures();
        test_enum_failures();
        test_handle_failures();
        test_struct_and_member_failures();
        test_named_argument_failures();
        test_class_failures();
        test_destroy_failures();
    } catch (const std::exception& error) {
        std::cerr << "parser test failure: " << error.what() << '\n';
        return 1;
    }

    std::cout << "parser tests passed\n";
    return 0;
}
