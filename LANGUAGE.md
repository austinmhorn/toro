
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

message := Message::text("hello")
quit := Message::quit
```

`::` denotes type-scoped enum variant access. `.` is reserved for instance and
member access, so the former `Message.text(...)` spelling is invalid. Other
type-scoped features such as static methods are not defined yet.

Payload-bearing variants require exactly one value of the declared type.
Payload-free variants are values without arguments. Each constructed variant has
the type of its containing enum, and values from different enums are distinct.

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

The handled expression must have an enum type. Case names must belong to that
enum and cannot repeat. Payload variants require a binding, payload-free variants
forbid one, and each binding is scoped and typed within its case block. Every
variant must be covered because wildcard/default cases are not defined yet.

`Result<T, E>` is enum-like, with `ok(T)` and `error(E)` cases. Its constructors
infer their payload types from an expected result type:

```toro
result: Result<int, string> = ok(10)
failure: Result<int, string> = error("bad")
```

A bare `ok(...)` or `error(...)` without an expected `Result<T, E>` type is
rejected. A `handle` over a result must exhaustively cover `ok` and `error`, with
bindings typed as `T` and `E` respectively.

Postfix `?` extracts `T` from a `Result<T, E>` expression. It is valid only in a
function returning another `Result` whose error type can accept `E`:

```toro
function checked_age() -> Result<int, string> {
    age := parse_age("28")?
    return ok(age)
}
```

Propagation does not unwrap nullable types and performs no automatic error
conversion. The C backend lowers concrete Results whose success and error types
are otherwise supported, evaluates a propagated expression once, and returns an
error Result immediately on failure.

### Structs and members

Struct fields have explicit types and may provide a default expression. During
construction, explicit defaults take priority over toro's type zero values.
Without an explicit default, `int`, `dec`, `string`, and `bool` fields receive
`0`, `0.0`, `""`, and `false`, respectively. Nullable fields receive `null`.
Structs may also declare public, non-virtual methods with normal `function`
syntax.

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
`self` has the containing class type and may access that class's private members.

```toro
class Player {
    public name: string
    private health: int = 100

    init(name: string) {
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

A class may declare one `init` method. When present, construction arguments bind
to its parameters. Construction allocates the object, initializes explicit field
defaults and toro zero/null values, then executes `init` exactly once with a valid
`self`. A required non-null field that has no safe default must be assigned on
every reachable path before `init` completes. Without `init`, construction keeps
using named or positional field arguments. `init(...) { ... }` is a dedicated
lifecycle declaration; spelling it as `function init(...)` is invalid.

A class may also declare one parameterless `destroy()` method, and neither
`init` nor `destroy` can declare a return type. The native backend runs
`destroy()` exactly once at final strong release, before owned fields and object
storage are freed. `self` and fields remain usable during that call. Initializer
overloading, base-initializer chaining, and failable initialization are not yet
supported.

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
explicit method modifiers. An abstract class may leave virtual methods bodyless.
Overrides must match an inherited virtual method exactly, and concrete classes
must implement inherited abstract methods. Implementing classes and structs must
provide matching public interface methods. The native backend dispatches class
virtual methods through deterministic per-concrete-class vtables. Runtime
interface dispatch remains a later phase.

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
value := identity<int>(10)
```

Generic function calls infer type arguments from positional or named arguments.
Repeated uses of one type parameter must infer a compatible single type, and
inferred types are substituted into return types, including nested generic return
types. Explicit type arguments must match the declared generic arity and replace
inference for those parameters.

Each interface constraint is checked after substitution. A concrete class or
struct satisfies a constraint by implementing that interface. Multiple
constraints separated by `+` must all be satisfied. Operations on an
unconstrained type parameter are rejected when the checker cannot prove them
valid.

Generic structs and classes form concrete, invariant instance types. Construction
may supply arguments explicitly or infer them by recursively matching field
types:

```toro
pair := Pair<int, string>(first: 10, second: "hello")
inferred := Pair(first: 10, second: "hello")
box: Box<int> = Box(value: 10)
```

The checker substitutes containing-type parameters through fields, method
parameters, method returns, and nested generic types. Thus `Box<int>.get()` has
type `int`, and `Store<User>.get_values()` may have type `List<User>`.
`Box<int>` and `Box<string>` are distinct and are not assignable to one another.
Constructor inference rejects conflicting or insufficient bindings, and generic
type constraints are checked after inference. Function inference uses the same
recursive matching for shapes such as `List<T>`, `Map<string, T>`, and
`Pair<A, List<B>>`. Monomorphization and runtime specialization remain deferred.

### Name resolution

Semantic analysis uses lexical scopes for the program, functions, blocks,
conditional branches, loops, and `handle` cases. Declarations must be unique in
their scope, while nested scopes may shadow enclosing declarations. Function
parameters, loop variables, and `handle` payload bindings are visible only in
their corresponding scope.

Functions and declared types are registered as symbols, and `print` is a
predefined symbol. `self` is available in class and struct methods. The current
passes validate inheritance, abstract methods, overrides, and interface
conformance, callable overloads, and generic function constraints.

### Primitive type checking

The initial type checker supports `int`, `dec`, `string`, and `bool`, with local
type inference for `:=`. Explicit primitive annotations and later assignments
must match exactly; `int` and `dec` are not implicitly converted.

Arithmetic and ordered comparisons require matching numeric operand types.
Equality requires compatible operands, logical operators require `bool`, and
`if` or `while` conditions must be `bool`. Function calls validate primitive
argument counts and types, and returns are checked against the function's
declared return type.

A function or concrete method with a declared return type must return a value on
every reachable path. A complete `if`/`else` tree guarantees a return only when
both branches do, and an exhaustive `handle` guarantees one only when every case
does. Nested blocks propagate this result. Loops are never assumed to execute,
even when their bodies return. Conversion overload bodies obey the same rule for
their target type. Functions without a return type have no completeness
requirement.

Concrete generic struct and class instances substitute their type arguments
through fields and methods. Runtime specialization remains deferred. Concrete
struct and class fields and methods are type checked, including inherited
members and interface method signatures.

### C generation

The initial backend emits C only after parsing, semantic analysis, and type
checking succeed. Its current subset includes primitive variables and
reassignment, arithmetic, comparisons, equality, logical expressions, unary
minus, functions, parameters, returns, calls, `if`/`else`, `while`, and
primitive `print(...)` calls. Non-generic structs may contain supported
primitive or non-generic struct fields and method signatures. Construction,
field access and assignment, nested field access, struct value copies, and
method calls are lowered to C. Methods receive an internal pointer to their
struct value so mutations through `self` update the source receiver. Enums lower
to tagged C unions with one tag per variant and one union member per payload.
Variant construction initializes the tag and optional payload explicitly.
Exhaustive `handle` statements lower to C `switch` statements, and payload
bindings are local to their case block. Enum values retain ordinary value-copy
semantics and may be passed to or returned from functions.

Each concrete `Result<T, E>` similarly lowers to its own tagged union with `ok`
and `error` payloads. Result parameters, returns, variables, copies, and
exhaustive `handle` statements use normal C value semantics. Postfix `?` stores
its operand in a temporary, checks the tag once, extracts the success payload,
or returns a freshly tagged error Result from the enclosing function.

Non-generic classes lower to heap-allocated C objects containing a strong
reference count, weak-control pointer, most-derived finalizer, and their instance
fields. Class variables store pointers, so
copying a class value preserves object identity rather than copying fields.
Construction creates one strong reference. Owned locals and class parameters
retain borrowed references, assignments release replaced references, returns
preserve the returned lifetime, and lexical-scope exit releases owned locals.
The generated release helper frees the object when its count reaches zero.
Class methods receive the same object pointer as `self`, so field mutations are
visible through every alias.

Single inheritance uses a prefix-compatible embedded-base object layout. A
derived object embeds its immediate base as its first member and adds its own
fields afterward, while base and derived references share the same header,
strong count, weak-control state, and allocation address.
Implicit derived-to-base conversion is therefore a pointer view, not a second
allocation. Releasing through any view invokes the stored most-derived finalizer.
That finalizer invalidates weak references, runs `destroy()` declarations from
most-derived class to base class, releases derived and inherited fields once,
and frees the single allocation. Inherited non-virtual methods are statically
called with the appropriate base view.

Strong class-reference fields participate in ARC: initialization and assignment
retain borrowed targets, replacement releases the previous target, and owner
destruction releases each strong field after `destroy()` returns. Nullable class
fields use `NULL` as their zero value. A field declared with `weak` must have a
nullable class type. It stores a reference-counted weak control block rather than
a raw object pointer, never increments the object's strong count, and loads the
control block's object pointer. Final strong release nulls that pointer before
running `destroy()`, so every surviving weak field immediately reads as `null`
without touching freed storage. Weak slots keep the control block alive until
they are reassigned, cleared, or their owner is destroyed.

ARC does not collect strong reference cycles. A strong parent-to-child edge plus
a weak child-to-parent edge cleans up, while two strong edges may leak.

Single-inheritance class hierarchies share one ARC/weak/finalizer header in the
embedded root object. When a hierarchy declares virtual methods, that header
also stores the concrete class vtable. Slots are identified by their declaring
class and declaration order; overrides reuse inherited slots. Generated thunks
start from the control block's most-derived object pointer and adjust it to the
class that implements the selected method. This preserves dynamic dispatch
through base references, parameters, returned references, and `self`, while
ordinary non-virtual methods remain statically dispatched. Lifecycle
`destroy()` declarations are never vtable entries and continue to run once in
derived-to-base order during final release.

Primitive types map directly to C:

```text
int     -> int64_t
dec     -> double
bool    -> bool
string  -> const char*
```

Generated names are deterministically prefixed to avoid collisions with C
keywords and backend helpers. Every struct compound literal initializes every
field: explicit source defaults take priority, while omitted `int`, `dec`,
`bool`, and `string` fields use `0`, `0.0`, `false`, and `""`. Omitted nested
struct values remain unsupported unless explicitly supplied.

Generic classes and structs, method overloads, conversions, runtime interfaces,
nullable non-class fields, collections, thread-safe ARC, and
cyclic-reference collection are not lowered yet. Enum and
Result payloads may use primitives, supported non-generic structs, supported
enums, or supported nested Results; other payload types produce a backend
diagnostic. Class-valued enum/Result payloads remain unsupported so ownership is
never guessed. Non-generic class `init` methods execute after field storage is
initialized. Method receivers and argument forms that cannot yet be owned safely
are materialized and released around the call; temporary class field access
remains rejected rather than lowered to invalid C.

The native toolchain can compile this generated source as C11. `toro build`
writes a persistent executable, defaulting to the source filename stem in the
current directory, while `toro run` compiles and executes from an isolated
temporary directory. The latter forwards standard output and error, preserves a
normal program exit status, reports compiler and process failures separately,
and removes its generated source and executable afterward. Compiler discovery
prefers `clang` and otherwise uses `cc` from `PATH`.

### Function and method overloads

Functions and methods may share a name when their ordered parameter types or
arity differ. Parameter names and return types do not distinguish overloads, so
declarations that differ only in either respect are duplicate signatures.

Calls first filter candidates by arity and named-argument compatibility, then by
parameter assignability. Exact concrete matches rank ahead of base-class or
interface-compatible matches. Equally ranked candidates are ambiguous, and no
implicit `int`/`dec` conversion is performed. An explicit `as` cast contributes
its concrete result type and can disambiguate a call. Inherited methods join the
derived overload set, while an identical derived signature remains subject to
`virtual` and `override` validation.

Generic candidates infer and substitute their type parameters, but remain ranked
beneath exact concrete and compatible subtype/interface overloads. Multiple
equally ranked generic candidates are ambiguous. Generic specialization is not
implemented. The built-in `print(...)` behavior is unchanged.

### Construction and members

Struct and class construction validates positional and named fields, rejects
duplicate or unknown fields, and checks field types. Primitive and nullable
fields may be omitted because they have zero values, and fields with explicit
defaults may also be omitted. A non-null named or generic field remains required
when the current type model cannot safely construct a default. Nested structs
are not implicitly default-constructed yet. Struct fields are public. Class
fields and methods are private by default and may be marked `public`; private
members are accessible only while checking the declaring class.

Member access returns the declared field type and may be chained. Member
assignment checks the field type. Method calls validate positional or named
arguments and return the declared result type; a method without a return type
cannot be used as a value. `self` is typed as its containing class or struct, so
method bodies receive the same field, method, and visibility checks as external
code.

Public inherited fields and methods participate in lookup. Private members remain
accessible only within their declaring type. Derived values are assignable to
their base class and implemented interfaces, but assignment in the opposite
direction is rejected.

Class `init` declarations provide constructor-specific parameters and must
definitely initialize required fields. Native single-inheritance construction
supports inherited fields when no base initializer call is required. Base
initializer chaining, runtime interface dispatch, monomorphization, and generic runtime
construction are not implemented yet.

### Nullable types

Types are non-nullable unless followed by `?`. A nullable type accepts its
non-null value or `null`, including user and generic types:

```toro
name: string? = null
user: User? = null
users: List<User>? = null
```

A `T` value may be assigned or passed to `T?`. A `T?` value cannot be assigned,
passed, or returned as `T` without future explicit handling. A bare inferred
declaration such as `value := null` is rejected because its intended nullable
type is unknown.

Nullable values may be compared with `null` using `==` or `!=`. They are not
implicitly boolean, so conditions require an explicit comparison:

```toro
if user != null {
}
```

Flow-sensitive narrowing and null-safe member access are not implemented yet.

### Explicit casts and conversions

The `as` operator performs explicit conversions and binds more tightly than
arithmetic operators:

```toro
decimal := 10 as dec
integer := 19.9 as int
result := integer as dec + 1.0
```

`int` and `dec` may be converted in either direction, but are never converted
implicitly. Once execution is implemented, `dec as int` will truncate toward
zero. Same-type casts are valid. A nullable `T?` cast to `T` does not unwrap the
value and is rejected.

Classes and structs may define one explicit conversion per target type:

```toro
class Player {
    name: string

    overload as string {
        return self.name
    }
}
```

The conversion runs only when requested with `player as string`; assignments,
arguments, and `print(player)` do not invoke it implicitly. General operator
overloads and code generation for conversions are not implemented yet.
