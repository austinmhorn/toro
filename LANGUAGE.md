
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

### Structs and members

Struct fields have explicit types and may provide a default expression. The
meaning of omitted defaults and zero values will be defined during semantic
analysis.

```toro
struct Player {
    name: string = ""
    health: int = 100
}
```

Calls accept positional arguments followed by named arguments. Member access
may be chained, and identifier or member-access expressions may be assignment
targets:

```toro
player := Player(name: "Austin")
player.health = 75
print(player.name)
```

### Classes

Classes contain ordered fields and methods. Members are private by default and
may be marked `public` or `private`. Methods use normal `function` syntax, and
`self` refers syntactically to the current instance.

```toro
class Player {
    public name: string
    private health: int = 100

    function init(name: string) {
        self.name = name
    }

    public function get_health() -> int {
        return self.health
    }

    function destroy() {
        print("player destroyed")
    }
}
```

`init` and `destroy` have no runtime semantics yet. A class may declare one
parameterless `destroy()` method, and it cannot declare a return type.

### Interfaces and inheritance

Classes may inherit from one base class after `:` and may implement multiple
comma-separated interfaces. Structs may implement interfaces but cannot inherit
from a base type.

```toro
interface Drawable {
    function draw()
}

abstract class Animal {
    virtual function speak()
}

class Dog : Animal implements Drawable {
    override function speak() {
        print("woof")
    }

    public function draw() {
        print("dog")
    }
}
```

Interface methods are signatures without bodies. `virtual` and `override` are
explicit method modifiers. A virtual method may omit its body; dispatch,
override validation, and interface conformance belong to later semantic phases.

### Generics

Functions, structs, classes, interfaces, and methods may declare generic type
parameters. A parameter may list interface constraints separated by `+`:

```toro
function process<T: Serializable + Comparable>(value: T) {
}
```

Type references may be nested, and calls may provide explicit type arguments:

```toro
index: Map<string, List<User>>
value := max<int>(10, 20)
```

Generic inference, constraint validation, and specialization belong to later
semantic and code-generation phases.
