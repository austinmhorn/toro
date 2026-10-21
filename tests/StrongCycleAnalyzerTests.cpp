#include "toro/Diagnostic.hpp"
#include "toro/EffectAnalyzer.hpp"
#include "toro/IRLowerer.hpp"
#include "toro/Lexer.hpp"
#include "toro/OwnershipSummaryAnalyzer.hpp"
#include "toro/Parser.hpp"
#include "toro/SemanticAnalyzer.hpp"
#include "toro/StrongCycleAnalyzer.hpp"
#include "toro/TypeChecker.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

void expect(bool condition, std::string_view message)
{
    if (!condition) throw std::runtime_error(std::string(message));
}

toro::IRProgram analyze(std::string_view source)
{
    auto program = toro::Parser(toro::Lexer(source).tokenize()).parse_program();
    toro::SemanticAnalyzer().analyze(program);
    toro::TypeChecker().check(program);
    auto ir = toro::IRLowerer().lower(program);
    toro::EffectAnalyzer().analyze(ir);
    toro::OwnershipSummaryAnalyzer().analyze(ir);
    toro::StrongCycleAnalyzer().analyze(ir);
    return ir;
}

std::string diagnostics(const toro::IRProgram& program)
{
    std::string result;
    for (const auto& diagnostic : program.diagnostics) {
        result += toro::format_diagnostic(diagnostic);
    }
    return result;
}

bool has_edge(
    const toro::IRProgram& program,
    std::string_view owner,
    std::string_view field,
    std::string_view target)
{
    return std::ranges::any_of(program.strong_ownership_edges,
        [&](const auto& edge) {
            return edge.owner_display == owner && edge.field_path == field
                && edge.target_display == target;
        });
}

void test_self_two_and_three_type_cycles()
{
    const auto self = analyze("class Node { public next: Node? }\n");
    expect(self.strong_reference_cycles.size() == 1
            && self.strong_reference_cycles.front().edges.size() == 1,
        "self-cycle was not reported exactly once");

    const auto pair = analyze(
        "class Parent { public child: Child? }\n"
        "class Child { public parent: Parent? }\n");
    expect(pair.strong_reference_cycles.size() == 1
            && pair.strong_reference_cycles.front().edges.size() == 2,
        "two-type cycle was not canonicalized to one diagnostic");
    expect(has_edge(pair, "Parent", "child", "Child")
            && has_edge(pair, "Child", "parent", "Parent"),
        "two-type ownership edges were incomplete");

    const auto triple = analyze(
        "class A { public b: B? }\n"
        "class B { public c: C? }\n"
        "class C { public a: A? }\n");
    expect(triple.strong_reference_cycles.size() == 1
            && triple.strong_reference_cycles.front().edges.size() == 3,
        "three-type cycle was not reported once");
}

void test_weak_edges_break_cycles()
{
    const auto one_weak = analyze(
        "class Parent { public child: Child? }\n"
        "class Child { public weak parent: Parent? }\n");
    expect(one_weak.strong_reference_cycles.empty()
            && !has_edge(one_weak, "Child", "parent", "Parent"),
        "weak back-reference participated in a strong cycle");

    const auto multiple_weak = analyze(
        "class A { public weak b: B? }\n"
        "class B { public weak c: C? }\n"
        "class C { public a: A? }\n");
    expect(multiple_weak.strong_reference_cycles.empty(),
        "multiple weak edges produced a cycle warning");
}

void test_inherited_and_generic_edges()
{
    const auto inherited = analyze(
        "class LinkBase { public tail: Tail? }\n"
        "class Head : LinkBase {}\n"
        "class Tail { public head: Head? }\n");
    expect(inherited.strong_reference_cycles.size() == 1
            && has_edge(inherited, "Head", "tail", "Tail"),
        "inherited strong field did not participate in the derived cycle");

    const auto generic = analyze(
        "class Node<T> { public next: Node<T>? }\n"
        "function observe_int(node: Node<int>) { print(node) }\n"
        "function observe_string(node: Node<string>) { print(node) }\n");
    expect(generic.strong_reference_cycles.size() == 2
            && has_edge(generic, "Node<int>", "next", "Node<int>")
            && has_edge(generic, "Node<string>", "next", "Node<string>"),
        "distinct concrete generic specialization identities were collapsed");
}

void test_collection_storage_edges()
{
    const auto list = analyze(
        "class Tree { public children: List<Node> }\n"
        "class Node { public parent: Tree? }\n");
    expect(list.strong_reference_cycles.size() == 1
            && has_edge(list, "Tree", "children[]", "Node"),
        "List element ownership did not participate in a cycle");

    const auto map = analyze(
        "class Registry { public nodes: Map<string, Node> }\n"
        "class Node { public registry: Registry? }\n");
    expect(map.strong_reference_cycles.size() == 1
            && has_edge(map, "Registry", "nodes[value]", "Node"),
        "Map value ownership did not participate in a cycle");

    const auto array = analyze(
        "class Group { public nodes: Array<Node> }\n"
        "class Node { public group: Group? }\n");
    expect(array.strong_reference_cycles.size() == 1
            && has_edge(array, "Group", "nodes[]", "Node"),
        "Array element ownership did not participate in a cycle");
}

void test_persistent_result_and_enum_payloads()
{
    const auto result = analyze(
        "class Owner {\n"
        "    public link: Result<Node, string> = error(\"empty\")\n"
        "}\n"
        "class Node { public owner: Owner? }\n");
    expect(result.strong_reference_cycles.size() == 1
            && has_edge(result, "Owner", "link::ok", "Node"),
        "persistent Result payload ownership was not represented");

    const auto enumeration = analyze(
        "enum Link {\n"
        "    node(Node)\n"
        "    empty\n"
        "}\n"
        "class Owner { public link: Link = Link::empty }\n"
        "class Node { public owner: Owner? }\n");
    expect(enumeration.strong_reference_cycles.size() == 1
            && has_edge(enumeration, "Owner", "link::node", "Node"),
        "persistent enum payload ownership was not represented");
}

void test_false_positive_controls()
{
    const auto acyclic = analyze(
        "class Parent { public child: Child? }\n"
        "class Child { public value: int }\n"
        "function inspect(parent: Parent, child: Child) {\n"
        "    print(parent.child == child)\n"
        "}\n");
    expect(acyclic.strong_reference_cycles.empty(),
        "acyclic fields or observation-only parameters produced a warning");

    const auto interface_only = analyze(
        "interface View { function value() -> int }\n"
        "class Item implements View {\n"
        "    public function value() -> int { return 1 }\n"
        "}\n"
        "class Holder { public view: View }\n");
    expect(interface_only.strong_reference_cycles.empty(),
        "non-owning interface relationship produced a warning");

    const auto impossible = analyze(
        "class Left { public right: Right }\n"
        "class Right { public left: Left }\n");
    expect(impossible.strong_reference_cycles.empty(),
        "unconstructible non-null relationship produced a type-only warning");
}

void test_diagnostics_are_actionable_deterministic_and_deduplicated()
{
    constexpr std::string_view source =
        "class A {\n"
        "    public b: B?\n"
        "    public other: B?\n"
        "}\n"
        "class B { public a: A? }\n";
    const auto first = analyze(source);
    const auto second = analyze(source);
    expect(first.strong_reference_cycles.size() == 1,
        "one strongly connected ownership component emitted duplicate cycles");
    expect(diagnostics(first) == diagnostics(second),
        "cycle diagnostics were not deterministic");
    const auto text = diagnostics(first);
    expect(text.contains("warning: possible strong reference cycle")
            && text.contains("[strong-reference-cycle]")
            && text.contains("possible strong reference cycle")
            && text.contains("ARC cannot reclaim")
            && text.contains("should be weak")
            && text.contains("declared at"),
        "cycle diagnostic omitted actionable context or source locations");
}

} // namespace

int main()
{
    try {
        test_self_two_and_three_type_cycles();
        test_weak_edges_break_cycles();
        test_inherited_and_generic_edges();
        test_collection_storage_edges();
        test_persistent_result_and_enum_payloads();
        test_false_positive_controls();
        test_diagnostics_are_actionable_deterministic_and_deduplicated();
    } catch (const std::exception& error) {
        std::cerr << "strong cycle analyzer test failure: " << error.what() << '\n';
        return 1;
    }
    std::cout << "strong cycle analyzer tests passed\n";
    return 0;
}
