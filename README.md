# toro

toro is a statically typed, native programming language focused on readable syntax, high performance, and progressive low-level control.

## Status

toro is currently under active development.

The first compiler implementation is written in C++23.
It currently loads `.to` source files and can tokenize their contents.
Parsing is an upcoming milestone.

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
./build/toro examples/hello.to
```

Print the source file's tokens:

```bash
./build/toro tokens examples/hello.to
```

Run the lexer tests:

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
