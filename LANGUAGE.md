
### `LANGUAGE.md`

We don't need the full language specification yet. Start with this:

```markdown
# toro Language Design

> This document tracks the evolving design of the toro programming language.

toro is currently pre-1.0. Language rules described here may change as the compiler develops.

---

## Philosophy

toro aims to make normal programming simple while allowing explicit control when performance matters.

Core principles:

1. Simple by default.
2. Native by default.
3. Performance can be explicitly constrained and inspected.
4. Complexity should be progressive.
5. Important runtime behavior should be understandable.
6. Readability is more important than minimizing character count.

---

## Basic Syntax

toro uses brace-delimited blocks.

Semicolons are not required.

```toro
function main() {
    value := 10

    if value > 5 {
        print("Hello, toro!")
    }
}