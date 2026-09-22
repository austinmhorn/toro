# Toro

Toro is a statically typed, native programming language focused on readable syntax, high performance, and progressive low-level control.

## Status

Toro is currently under active development.

The first compiler implementation is written in C++23.

## Goals

Toro aims to provide:

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
    message := "Hello, Toro!"
    print(message)
}