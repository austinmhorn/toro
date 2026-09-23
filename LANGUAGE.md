
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

### Logical Operators

toro uses the keywords `and` and `or` rather than symbolic logical operators.
`and` has higher precedence than `or`; equality and comparison operators have
higher precedence than both.

These operators will short-circuit when execution and code generation are implemented:

- `false and expression` does not evaluate `expression`.
- `true or expression` does not evaluate `expression`.

### Loops

Collection iteration uses `for item in collection`, and conditional loops use
`while condition`. `stop` exits the nearest enclosing loop; `continue` skips to
its next iteration.

### Enums and `handle`

Enums may contain payload-free variants or variants carrying one value type:

```toro
enum Message {
    text(string)
    quit
}
```

The general-purpose `handle` statement matches an expression against ordered
variant cases. A payload-bearing case may bind the payload for use in its block:

```toro
handle message {
    text(value) {
        print(value)
    }

    quit {
        return
    }
}
```

Wildcard cases and exhaustiveness checking are not defined yet.
