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

The `check` command reports name-resolution errors, type mismatches, invalid
conditions and returns, bad construction fields, invalid member access, method
argument errors, and visibility violations. Construction supplies zero values
for primitive and nullable fields, honors explicit field defaults, and requires
non-null named fields that cannot be safely defaulted. Generic member
specialization, runtime virtual dispatch, and flow-sensitive null narrowing are
not implemented yet. Functions and methods may overload by parameter types and
arity; resolution prefers exact matches over base/interface compatibility and
reports missing or ambiguous matches. Explicit `as` casts support numeric
conversions and user-defined class or struct conversions. Generic function calls
infer type arguments, accept validated explicit type arguments, substitute return
types, and enforce interface constraints.

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
