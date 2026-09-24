# toro

toro is a statically typed, native programming language focused on readable syntax, high performance, and progressive low-level control.

## Status

toro is currently under active development.

The first compiler implementation is written in C++23.
It currently loads and tokenizes `.toro` source files and can parse expressions,
variable declarations, assignments, calls, functions, returns, blocks, conditional
control flow, loops, enums, `handle` statements, structs, named calls, and member
access and assignment, classes, single inheritance, interfaces, virtual or
override methods, and generic declarations and type references into an AST.
The semantic analysis pass provides lexical scopes, symbol registration, and
basic name resolution. A separate type-checking pass infers primitive local
types and validates expressions, assignments, functions, struct/class
construction, fields, methods, visibility, inheritance, interface conformance,
abstract classes, nullable types, and explicit casts.
Generic struct and class instances retain their concrete type arguments. The
checker validates explicit or inferred generic construction, constraints, and
substituted field and method types, including recursively nested generic types.
Enum variants are typed constructors, and `handle` validates case payload
bindings, variant identity, duplicate cases, and exhaustive coverage.
Enum variants use type-scoped access such as `Message::text("hello")` and
`Message::quit`; `.` remains exclusively instance/member access.
A minimal backend can emit portable C for the currently supported primitive
procedural subset, non-generic value structs, and enums with exhaustive
`handle` statements.

## Build

```bash
cmake -S . -B build
cmake --build build
```

## Usage

Print the compiler version:

```bash
./build/toro --version
```

Load and print a toro source file:

```bash
./build/toro examples/hello.toro
```

Print the source file's tokens:

```bash
./build/toro tokens examples/hello.toro
```

Parse a file containing one expression and print its AST:

```bash
./build/toro ast-expression examples/expression.toro
```

Parse top-level statements and print their AST:

```bash
./build/toro ast examples/variables.toro
```

Parse conditional control flow:

```bash
./build/toro ast examples/conditionals.toro
```

Parse collection and conditional loops:

```bash
./build/toro ast examples/loops.toro
```

Parse enums and pattern-handling cases:

```bash
./build/toro ast examples/enums.toro
```

Parse structs, construction calls, and member operations:

```bash
./build/toro ast examples/structs.toro
```

Parse classes, visibility, and lifecycle methods:

```bash
./build/toro ast examples/classes.toro
```

Parse interfaces and inheritance declarations:

```bash
./build/toro ast examples/inheritance.toro
```

Parse generic declarations, constraints, types, and calls:

```bash
./build/toro ast examples/generics.toro
```

Run semantic name resolution and primitive type checking on a source file:

```bash
./build/toro check examples/hello.toro
```

Generate C after parsing and checking a source file:

```bash
./build/toro emit-c examples/hello.toro
./build/toro emit-c examples/structs.toro
./build/toro emit-c examples/enums.toro
```

Build a native executable with the host C compiler:

```bash
./build/toro build examples/hello.toro
./build/toro build examples/hello.toro -o /tmp/toro-hello
```

Without `-o`, the executable is written to the current directory using the
source filename stem, such as `./hello`. The native toolchain prefers `clang`
and falls back to `cc`, compiling the generated source as C11.

Build and run through temporary artifacts:

```bash
./build/toro run examples/hello.toro
./build/toro run examples/structs.toro
./build/toro run examples/enums.toro
```

`run` forwards program output and returns its exit status. Its temporary C source
and executable are removed after the program finishes or compilation fails.

The `check` command reports name-resolution errors, type mismatches, invalid
conditions and returns, bad construction fields, invalid member access, method
argument errors, and visibility violations. Construction supplies zero values
for primitive and nullable fields, honors explicit field defaults, and requires
non-null named fields that cannot be safely defaulted. Generic instances are
invariant, so different concrete argument lists are distinct types. Runtime
virtual dispatch and flow-sensitive null narrowing are not implemented yet.
Functions and methods may overload by parameter types and
arity; resolution prefers exact matches over base/interface compatibility and
reports missing or ambiguous matches. Explicit `as` casts support numeric
conversions and user-defined class or struct conversions. Generic function calls
infer type arguments, accept validated explicit type arguments, substitute return
types, and enforce interface constraints. Inference recursively matches nested
types such as `List<T>` and `Map<string, List<T>>`.
Enum construction and exhaustive `handle` checking are also part of this pass.
`ok(value)` and `error(value)` construct an expected `Result<T, E>`, while
postfix `?` extracts the success type and propagates a compatible error from a
function returning `Result`.
Functions and concrete methods with declared return types must return on every
reachable path. Complete `if`/`else` trees and exhaustive returning `handle`
statements satisfy this requirement; loops are not assumed to execute.

The initial C backend lowers primitive variables, expressions, functions,
returns, calls, `if`/`else`, `while`, primitive `print(...)` calls, and
non-generic structs. Struct construction emits every field explicitly, applying
source defaults or toro primitive zero values. Field access, chained access,
assignment, value copies, and struct methods use native C value semantics;
methods lower to prefixed functions with an internal receiver pointer. It uses
deterministically prefixed C identifiers and emits a C entry-point wrapper for a
toro `main`. Enums lower to deterministic tagged unions; construction selects a
tag and payload, while exhaustive `handle` statements lower to `switch` blocks
with case-scoped payload bindings. Generic structs, interfaces, conversion
overloads, nullable fields, `Result<T, E>`, and other runtime types outside this
subset fail with a backend diagnostic instead of producing partial or incorrect
C.

## Logical operators

toro uses the keywords `and` and `or` for logical expressions. `and` binds more
tightly than `or`, while equality and comparison operators bind more tightly than
both. These operators will use short-circuit semantics when execution and code
generation are implemented:

```text
false and expression  // expression not evaluated
true or expression    // expression not evaluated
```

The symbolic forms `&&` and `||` are not part of toro.

Run the test suite:

```bash
ctest --test-dir build --output-on-failure
```

## Goals

toro aims to provide:

- Simple, readable syntax
- Native compilation
- Static typing with intuitive type inference
- Object-oriented and value-oriented programming
- Generics
- Explicit error handling
- Automatic memory management
- Compiler-enforced performance contracts
- Low-level control when explicitly requested

## Example

```toro
function main() {
    message := "Hello, toro!"
    print(message)
}
```
