# toro

toro is a statically typed, native programming language focused on readable syntax, high performance, and progressive low-level control.

## Status

toro is currently under active development.

The first compiler implementation is written in C++23.
It currently loads and tokenizes `.toro` source files and can parse expressions,
variable declarations, assignments, and basic call-expression statements into an AST.

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
