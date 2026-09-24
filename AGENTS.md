# AGENTS.md — Toro project instructions

This file defines persistent implementation instructions for agents working in this repository.

## Project identity

- `Toro` is the human-readable proper name. Use it in normal prose and comments
  when referring to the language or project.
- `toro` is the canonical technical and machine-facing spelling. Keep it
  lowercase for executables, CLI commands, source extensions, paths,
  directories, namespaces, generated names, CMake targets, project-controlled
  metadata naming the technical artifact, and literal diagnostics/output
  referring to the executable or compiler identifier. Implementation-language
  identifiers may follow their native naming conventions as described below.
- Do not use `TORO` as branding. Uppercase is acceptable only where an external
  convention requires it.
- Source files use the `.toro` extension. Do not introduce or restore `.to`.
- The compiler implementation is C++23.
- The current native backend emits C11 and invokes a system C compiler.
- Prefer `#pragma once` for headers.

### Host-language identifier conventions

Do not mechanically lowercase implementation identifiers merely because they contain the project name.

Use the native naming conventions of the host language or build system when appropriate.

Examples:

```text
Toro                    human-facing proper noun
toro                    CLI/executable/package/path/namespace/generated prefix
namespace toro           correct C++ namespace
.toro                    correct source extension
toro build               correct CLI spelling
TORO_TEST_C_COMPILER     valid C/C++ preprocessor or compile-time identifier
kToroVersion             valid C++ identifier if consistent with project style
```

Uppercase `TORO` is prohibited as human-facing branding, but it is allowed inside idiomatic implementation identifiers such as preprocessor macros, compile-time constants, environment-style variables, or build-system identifiers.

Do not rename conventional host-language identifiers solely to enforce branding capitalization.

## Authoritative project documents

Treat these files as canonical project context:

1. `AGENTS.md` — agent workflow and implementation guardrails.
2. `LANGUAGE.md` — canonical Toro syntax and language semantics.
3. `docs/ARCHITECTURE.md` — current compiler/runtime architecture and planned architectural direction.
4. `ROADMAP.md` — implementation sequence and milestone priorities.
5. `docs/DESIGN_PHILOSOPHY.md` — rationale and long-term language thesis.
6. `README.md` — public-facing overview and current usage.

If these files conflict, do not silently choose one. Report the contradiction before making a design-changing implementation.

## Workflow

- Work only inside the Toro repository unless explicitly instructed otherwise.
- Do not commit, amend, rebase, tag, or push unless explicitly instructed.
- Preserve the user's manual Git workflow.
- Make incremental changes with focused scope.
- Prefer the smallest implementation that satisfies the milestone cleanly.
- Do not introduce unrelated refactors during feature work.
- Do not weaken tests merely to make them pass.
- Keep diagnostics clear, deterministic, and source-located where practical.
- Keep `README.md`, `LANGUAGE.md`, `ROADMAP.md`, and architecture/design docs aligned when a milestone materially changes documented behavior.

## Language-design guardrails

Toro should remain readable and unsurprising.

- If a basic feature requires a paragraph of syntax rules to understand, reconsider the design.
- Prefer one canonical spelling over aliases.
- Do not add compatibility aliases for rejected syntax unless explicitly requested.
- Preserve the distinction:
  - `:=` declares.
  - `=` reassigns.
  - `.` accesses an instance/value member.
  - `::` accesses a type-scoped enum variant.
- Use `function`, not `fn`.
- Use `dec`, not `float`/`double`, in normal Toro source.
- Use `and` / `or`, not `&&` / `||`.
- Use `stop`, not `break`.
- `init(...) {}` and `destroy() {}` are lifecycle declarations, not ordinary methods.
- Class references are strong by default.
- `weak` is explicit and currently applies to nullable class-reference fields.
- Do not introduce a `strong` keyword merely to restate the default.
- Do not introduce a general `pointer` type as a synonym for class references. A future pointer type, if any, should represent explicitly low-level memory/FFI semantics.

## Core semantic model

- `struct` is a value type.
- `class` is a reference type with identity.
- Interfaces describe capabilities.
- Classes support single inheritance.
- Classes/structs may implement multiple interfaces.
- `virtual` / `override` are explicit.
- `init` is optional:
  - classes with `init` bind construction arguments to the initializer;
  - classes without `init` use field-based construction.
- `destroy` runs automatically at final class destruction and is not called manually.
- Common value fields have zero values:
  - `int` -> `0`
  - `dec` -> `0.0`
  - `string` -> `""`
  - `bool` -> `false`
  - `T?` -> `null`
- Non-null class/reference fields require a valid explicit/default/initializer-proven value.
- `Result<T,E>` uses `ok(value)` and `error(value)`.
- Postfix `?` means Result error propagation only; it is not nullable unwrapping.
- Enum variants use `Type::variant` / `Type::variant(payload)`.
- Data-carrying enums are tagged unions under the same `enum` construct.
- User-defined conversions are explicit via `overload as Type`; custom conversions are never implicit.

## Memory and performance philosophy

Toro should have **ownership analysis, not ownership programming**.

Current runtime correctness comes first. The current C backend may use straightforward ARC even when a future optimizer could remove it.

Long term:

- Values remain values and should not use ARC.
- Class/reference semantics remain stable even if the compiler changes storage strategy.
- The compiler should infer moves, borrows, escape behavior, and stack allocation where safe.
- ARC is a semantic fallback for shared reference lifetime, not the identity of the language.
- Future ownership/escape/borrow analysis belongs behind a toro IR layer, not as ad-hoc logic spread through the C generator.
- Avoid source-level ownership syntax unless real programs prove it necessary.
- Do not prematurely implement arenas, atomic ARC, ownership annotations, or other advanced memory features before the roadmap reaches them.

`performance function` is a future compiler-enforced execution contract, not merely an optimization hint. Its eventual implementation should be transitive through the call graph and capable of rejecting prohibited behavior such as general heap allocation, blocking I/O, hidden synchronization, escaping temporaries, or calls that violate the contract.

Performance/memory decisions should eventually be observable and explainable through Toro tooling.

## Architecture guardrails

Current pipeline:

`source -> lexer -> parser/AST -> semantic analysis -> type checking -> C generator -> native compiler`

Future pipeline should evolve toward:

`source -> frontend -> toro IR -> ownership/escape/borrow analysis -> ARC insertion/elision -> performance validation -> backend`

Do not tightly couple new language semantics to C-specific implementation details if a clean backend abstraction is practical.

Correctness is more important than premature optimization.

## Testing and verification

For compiler changes, normally run:

```bash
rm -rf build
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
./build/toro --version
git diff --check
git status
```

Also run the relevant `toro check`, `emit-c`, `build`, and/or `run` examples for the milestone.

Preserve existing regression coverage.

## Reporting

At the end of a task, report:

- files changed;
- behavior implemented;
- tests added/updated;
- verification results;
- unsupported/deferred cases;
- deviations from requested scope;
- Git status.

Do not commit or push unless explicitly instructed.
