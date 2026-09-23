# toro Roadmap

> **Status:** Initial language roadmap  
> **Scope:** v0.1 → v1.0  
> **Primary goal:** Build a readable native programming language that is simple in ordinary code, powerful when abstraction is useful, and capable of compiler-enforced performance guarantees when predictability matters.

---

## 1. toro's Design Principles

toro should remain guided by a small set of rules throughout development:

1. **Readable over terse.** toro prefers `function`, `public`, `private`, `handle`, and `stop` when the longer word makes intent clearer.
2. **Simple by default.** Common code should require as little ceremony as possible.
3. **Explicit when ambiguity matters.** toro infers obvious types and behavior, but provides explicit syntax such as `x as int` when the programmer needs control.
4. **Native by default.** toro compiles to standalone native executables.
5. **Safe defaults, controlled escape hatches.** Bounds checking, non-nullable types, deterministic ownership, and explicit casts should be the normal path; lower-level control can be added where needed.
6. **Abstraction is allowed. Hidden cost must be explainable.** Classes, virtual dispatch, generics, and closures are useful; toro should make their runtime cost observable.
7. **Performance is a contract, not a wish.** `performance function` must change what code is allowed to do and enable stronger compiler/runtime guarantees.
8. **Stable project structure.** Imports should remain stable when files move inside a project.
9. **Private by default.** Public API is deliberate and uses the `public` keyword.
10. **Grow the language deliberately.** A feature should solve a real problem before becoming part of toro.

---

## 2. Core Syntax Decisions Already Locked In

```toro
function add(a: int, b: int) -> int {
    return a + b
}

function main() {
    x := 10
    x = 20

    price := 19.99
    explicit_price: dec = 24.50

    if x > 10 {
        print("toro works!")
    }
}
```

### Variables and types

```toro
x := 10              // mutable, inferred int
price := 19.99       // mutable, inferred dec
name: string = ""    // explicit type

x = 20               // reassignment
print(x as int)      // explicit cast / overload disambiguation
```

Planned later:

```toro
const max_users := 100
```

### Zero/default values

Common value types receive sensible defaults when not explicitly initialized:

```text
int      -> 0
dec      -> 0.0
bool     -> false
string   -> ""
T?       -> null
```

A non-nullable class reference must still refer to a real object.

### Nullability

```toro
user: User      // never null
maybe: User?    // User or null
```

### Visibility

Declarations are private by default.

```toro
function helper() {
}

private function explicit_helper() {
}

public function start() {
}
```

### Control flow

```toro
if condition {
}

else {
}

while running {
}

for user in users {
    if user.disabled {
        continue
    }

    if user.is_admin {
        stop
    }
}
```

`stop` exits the nearest enclosing loop.

### Result handling

```toro
function load_user(id: int) -> Result<User, Error> {
    ...
}

handle load_user(42) {
    ok(user) {
        print(user.name)
    }

    err(error) {
        print(error)
    }
}
```

Concise propagation:

```toro
function load_profile(id: int) -> Result<Profile, Error> {
    user := load_user(id)?
    settings := load_settings(user)?

    return Profile(user, settings)
}
```

### Enums and variants

```toro
enum Message {
    text(string)
    image(Image)
    quit
}

handle message {
    text(value) {
        print(value)
    }

    image(value) {
        display(value)
    }

    quit {
        stop
    }
}
```

`handle` is toro's general pattern-handling construct, not only an error-handling keyword.

---

# Version Roadmap

---

## v0.1 — toro Exists

### Theme

**Build the smallest complete compiler that can turn useful toro source into a native executable.**

The goal of v0.1 is not feature completeness. The goal is proving the complete compilation pipeline.

### Compiler pipeline

```text
toro source
    ↓
Lexer
    ↓
Tokens
    ↓
Parser
    ↓
AST
    ↓
Semantic analysis
    ↓
Type checking
    ↓
C code generation
    ↓
clang
    ↓
Native executable
```

### Language features

- Primitive/common types:
  - `int`
  - `dec`
  - `bool`
  - `string`
- Variable declaration with `:=`
- Reassignment with `=`
- Explicit type annotations with `:`
- Explicit cast/disambiguation with `as`
- Functions using `function`
- Explicit parameter and return types
- `return`
- `if` / `else`
- `while`
- `for x in collection`
- `stop`
- `continue`
- Basic operators:
  - arithmetic: `+ - * / %`
  - comparison: `== != < <= > >=`
  - boolean: `&& || !`
- Strings and string literals
- Basic interpolation if implementation cost remains reasonable
- Basic `struct`
- Basic `class`
- `Array<T>`
- `List<T>`
- Zero-based indexing
- Bounds checking by default
- Nullable `T?`
- `null`
- `Result<T, E>`
- `?` propagation
- Basic enums / variants
- `handle`
- Basic module imports
- `public` / `private`
- `print()` in the initial standard library

### Object behavior

At v0.1, classes may remain intentionally minimal:

- Fields
- Methods
- Reference identity
- Basic construction
- No inheritance yet
- No virtual dispatch yet
- No interfaces yet

Structs are value types.

### Collections

Initial collection model:

```text
Array<T>    fixed-size contiguous collection
List<T>     growable contiguous collection
```

`List<T>` assignment is reference-backed and cheap:

```toro
b := a
```

Explicit independent copying uses:

```toro
b := a.clone()
```

### Modules

Use stable project/module-root imports:

```toro
import game.player
import net.http
```

Avoid fragile relative-import chains as the primary module model.

### CLI

Initial command set:

```bash
toro run main.toro
toro build main.toro
toro check main.toro
```

### Exit criteria

v0.1 is complete when this program can compile and run natively:

```toro
function add(a: int, b: int) -> int {
    return a + b
}

function main() {
    x := add(10, 20)

    if x > 20 {
        print("toro works!")
    }
}
```

And:

```bash
toro run main.toro
```

prints:

```text
toro works!
```

---

## v0.2 — Functions Feel Like toro

### Theme

**Complete toro's function model and make APIs pleasant to call.**

### Features

- Function overloading
- Deterministic overload resolution
- Exact concrete matches preferred over generic/fallback matches
- No implicit numeric conversion solely to satisfy an overload
- Ambiguous calls require explicit `as`
- Default arguments
- Named arguments
- Positional arguments must precede named arguments
- Function values / first-class functions
- Function types
- Improved compiler diagnostics for call mismatches

### Examples

```toro
function print(value: int) {
}

function print(value: dec) {
}

print(10)
print(10 as dec)
```

```toro
function connect(
    host: string,
    port: int = 8080,
    secure: bool = false
) {
}

connect("localhost")
connect("localhost", secure: true)
```

### Exit criteria

- Overload resolution is deterministic and documented.
- Named/default arguments work across normal functions and methods.
- Functions can be stored and passed as values.

---

## v0.3 — Object Model and ARC

### Theme

**Deliver toro's full everyday object model with deterministic automatic lifetime management.**

### Struct model

- Structs are value types.
- Structs can contain methods.
- Structs can implement interfaces later.
- Struct construction supports named fields.
- Struct fields receive zero/default values where appropriate.

### Class model

- Classes are reference types with identity.
- Automatic Reference Counting (ARC) manages normal class lifetimes.
- Normal references are strong references.
- Weak references do not keep objects alive.

```toro
class Child {
    weak parent: Parent?
}
```

When the target of a weak reference is destroyed, the weak reference becomes `null`.

### Lifecycle

toro's deterministic cleanup method is:

```toro
function destroy() {
    ...
}
```

Example:

```toro
class FileHandle {
    private handle: int

    function destroy() {
        close(handle)
    }
}
```

`destroy()` runs when ARC determines that the class instance is no longer owned.

### Construction

Classes use `init`:

```toro
class Player {
    name: string
    health: int = 100

    function init(name: string) {
        self.name = name
    }
}
```

Structs receive a generated field initializer unless overridden later by language design.

### ARC compiler work

- Retain/release insertion
- Escape analysis groundwork
- Weak-reference runtime support
- Cycle documentation and diagnostics
- ARC optimization passes begin

### Exit criteria

- Normal toro class code requires no manual free/delete.
- Deterministic `destroy()` behavior works reliably.
- Common parent/child graphs can use weak backreferences safely.

---

## v0.4 — Inheritance, Interfaces, and Dynamic Polymorphism

### Theme

**Add OO power without importing C++'s inheritance complexity.**

### Features

- Single class inheritance
- No multiple class inheritance
- Multiple interfaces
- Explicit `virtual`
- Explicit `override`
- Abstract classes
- Abstract methods
- Interface implementation by classes
- Interface implementation by structs
- Runtime virtual/interface dispatch
- Clear compiler diagnostics for invalid overrides

### Example

```toro
interface Drawable {
    function draw()
}

abstract class Animal {
    public name: string

    virtual function speak() {
        print("...")
    }
}

class Dog : Animal implements Drawable {
    override function speak() {
        print("woof")
    }

    function draw() {
        ...
    }
}
```

### Philosophy

- Inheritance models identity.
- Composition models capability/state when inheritance is unnecessary.
- Interfaces model contracts/capabilities.

### Exit criteria

toro can express traditional OO designs while keeping class inheritance trees structurally simple.

---

## v0.5 — Generics and Complete Collections

### Theme

**Bring C++-class reusable abstraction to toro without template metaprogramming complexity.**

### Generic features

- Generic functions
- Generic structs
- Generic classes
- Generic type inference
- Interface-based generic constraints
- Multiple constraints
- Monomorphization

```toro
function max<T: Comparable>(a: T, b: T) -> T {
    ...
}
```

```toro
struct Pair<A, B> {
    first: A
    second: B
}
```

### Explicit non-goals for this phase

- SFINAE-style behavior
- Template-template parameter complexity
- Arbitrary compile-time template recursion
- Large template metaprogramming subsystem

Specialization may be designed later only if concrete use cases justify it.

### Collections

Complete initial collection family:

```text
Array<T>
List<T>
Map<K, V>
```

Define and document:

- Copy/reference behavior
- `.clone()` behavior
- Iteration contracts
- Mutation behavior
- Bounds checking
- Capacity/growth rules
- Hash/equality requirements for maps

### Exit criteria

toro has efficient reusable containers and generic algorithms with concrete native specialization.

---

## v0.6 — Closures and Expressive Data Processing

### Theme

**Make functional-style APIs practical without hiding runtime cost.**

### Features

- Closures
- Lexical capture
- Capture analysis
- Anonymous function syntax
- Collection algorithms such as:
  - `map`
  - `filter`
  - `find`
  - `any`
  - `all`
  - sorting with function values
- Diagnostics that explain closure capture behavior

Possible syntax:

```toro
numbers.map(function(x: int) -> int {
    return x * 2
})
```

Potential shorthand may be explored later, but should not be added merely for terseness.

### Compiler/runtime goals

toro should be able to explain whether a closure:

- Captures nothing
- Captures by value/reference
- Escapes its declaring scope
- Requires heap storage
- Can remain stack allocated

### Exit criteria

Closures are practical for everyday APIs while their allocation/lifetime costs remain inspectable.

---

## v0.7 — Project Tooling and Developer Experience

### Theme

**Make toro feel like one coherent toolchain rather than only a compiler.**

### Project structure

Typical project:

```text
my-project/
├── toro.toml
├── toro.lock
├── src/
│   └── main.toro
├── tests/
└── examples/
```

### CLI expansion

```bash
toro new my-project
toro run
toro build
toro check
toro test
toro fmt
```

### Formatter

- Official formatter
- Deterministic output
- Minimal configuration initially
- Formatting should reflect toro's readability philosophy

### Testing

Introduce a built-in test workflow.

Exact test declaration syntax can be designed during this milestone rather than prematurely.

### Configuration

`toro.toml` becomes the canonical project manifest.

Potential responsibilities:

- Project metadata
- Build targets
- Dependencies
- Compiler options
- Source roots

`toro.lock` records resolved dependency versions when package management arrives.

### Diagnostics

Significant focus on compiler error quality:

```text
error: call to 'process' is ambiguous

possible matches:
  process(value: Foo)
  process(value: Bar)

help: specify the intended type:
  process(value as Foo)
```

### Exit criteria

A new developer can create, format, test, build, and run a toro project using only the `toro` CLI.

---

## v0.8 — Performance Functions

### Theme

**Introduce toro's defining performance contract.**

`performance function` is not an optimization hint. It is a restricted execution model whose guarantees are enforced by the compiler.

```toro
performance function update(world: World, dt: dec) {
    physics.update(world, dt)
    units.update(world, dt)
}
```

### Initial contract

A `performance function` should prohibit or tightly control:

- General heap allocation
- Blocking file/network I/O
- Sleeping/waiting
- Hidden locks/synchronization
- Escaping temporary objects
- Hidden allocating conversions
- Calls to functions that violate the same performance contract

The contract is transitive through the call tree.

### Compiler benefits

Because the compiler knows more about execution behavior, it may safely apply stronger strategies such as:

- Borrowed parameters
- ARC retain/release elision
- Stack allocation
- Frame/arena temporary allocation
- Aggressive escape analysis
- Inlining
- Constant propagation
- Vectorization
- Removal of provably unnecessary runtime work

### Temporary storage

Explore a toro-native temporary allocation model for performance functions, such as frame/arena storage that can be discarded as a unit when execution exits the performance scope.

Exact syntax should be chosen from implementation experience rather than guessed now.

### Inspection tooling

Introduce:

```bash
toro inspect <symbol>
```

Potential output:

```text
update(World, dec)

heap allocations       0
blocking operations    0
dynamic dispatches     3
large copies           0
stack                  1.4 KB
```

Potential companion command:

```bash
toro build --explain-performance
```

### Important non-guarantee

`performance function` does **not** promise good algorithmic complexity.

toro can guarantee execution characteristics; it cannot make an O(n³) algorithm inherently fast.

### Exit criteria

Performance-sensitive toro code can receive useful compiler-enforced guarantees that ordinary functions do not provide.

---

## v0.9 — Systems Interop, Packaging, and Release Candidate

### Theme

**Make toro practical outside toy programs and stabilize everything needed for a public ecosystem.**

### C interoperability

Design a clear C ABI bridge for:

- Calling C functions
- Linking C libraries
- Exposing toro functions to C where practical
- Mapping primitive layouts
- Explicit raw-pointer boundaries

Low-level/unsafe syntax should only be added if implementation experience proves it necessary and the semantics can remain clear.

### Explicit-width numeric control

Normal toro remains intuitive:

```toro
count := 10
price := 12.5
```

Advanced users gain stable explicit-width numeric types where needed.

The exact naming of low-level decimal/floating representations should be finalized only after backend and FFI requirements are concrete.

### Package management

Introduce dependency resolution through the toro CLI.

Possible workflow:

```bash
toro add example.package
toro remove example.package
toro update
```

### Package registry

Decide whether the initial public release includes:

- An official registry
- Git-based dependencies first
- Both

Do not build registry infrastructure before the package format and dependency resolver are stable.

### Compatibility work

- Freeze major syntax decisions
- Identify accidental inconsistencies
- Deprecate experimental syntax
- Improve error compatibility/migration messages
- Establish semantic-versioning expectations for toro packages

### Performance validation

Create a benchmark suite covering:

- Function calls
- ARC behavior
- Virtual dispatch
- Generics
- Collections
- Closures
- Performance functions
- C interop

### Exit criteria

v0.9 is the **1.0 release candidate line**. New language features should largely stop here; focus shifts to correctness, tooling, documentation, performance, and compatibility.

---

## v1.0 — toro Stable

### Theme

**A language people other than its creator can confidently adopt.**

### v1.0 guarantees

toro 1.0 should provide:

- A documented stable grammar
- A documented type system
- Stable common primitive semantics
- Stable object/value model
- Stable ARC behavior
- Stable nullable semantics
- Stable `Result<T, E>` behavior
- Stable enum/variant and `handle` semantics
- Stable inheritance/interface rules
- Stable generic rules
- Stable module/import behavior
- Stable collection contracts
- Stable `performance function` contract
- Stable C interop surface
- Stable project manifest format
- Stable CLI commands
- Official formatter
- Official test runner
- Dependency management
- Clear compatibility policy

### Required tooling

```bash
toro new
toro run
toro build
toro check
toro test
toro fmt
toro add
toro remove
toro update
toro inspect
```

### Documentation

Before v1.0, publish:

- Language tour
- Full language reference
- Standard library reference
- Compiler/tooling guide
- C interop guide
- Performance-function guide
- Memory-management/ARC guide
- Migration guide from pre-1.0 toro
- Examples repository

### Platform goal

The exact support matrix should be based on real implementation bandwidth, but toro 1.0 should avoid claiming platforms that are not continuously tested.

A reasonable initial target is desktop/server native compilation on major 64-bit platforms, expanding only when CI and runtime support are reliable.

### Performance goal

Do not define toro 1.0 success as "beats C++ in every benchmark."

The goal is:

> toro should produce predictable native code, offer low-overhead abstractions, and give programmers a clear path from simple application code to tightly controlled performance-sensitive code without changing languages.

### Stability promise

Before 1.0, breaking changes are expected when they materially improve the language.

After 1.0:

- Breaking syntax/semantic changes require a deliberate versioning process.
- Compiler diagnostics should help users migrate when possible.
- Standard-library compatibility becomes a first-class responsibility.

---

# Features Intentionally Deferred Beyond v1.0

These may be excellent toro features, but they should not delay 1.0 unless implementation experience demonstrates that one is essential:

- `async` / `await`
- Native coroutine model
- Goroutine-style concurrency
- Advanced compile-time metaprogramming
- Macros
- Reflection
- Advanced generic specialization
- SIMD language syntax
- GPU/kernel language extensions
- Distributed programming primitives
- Built-in actor model
- Hot reload
- JIT compiler
- Self-hosting toro compiler
- Dedicated LLVM backend if the C backend remains adequate through early development
- Full IDE/debugger implementation

These should be evaluated from real toro programs rather than speculative design.

---

# Implementation Strategy

## Initial compiler language

Implement the first toro compiler in **C++23**.

Reasons:

- Native compiler/tooling ecosystem
- Fine control over memory and data representation
- Easy integration with Clang/toolchains
- Suitable for building lexer/parser/AST/compiler infrastructure
- Lets toro's implementation eventually serve as a direct comparison point against the complexity toro intends to simplify

## Initial backend

Use a C backend first:

```text
toro AST
   ↓
Typed toro IR / semantic representation
   ↓
Generated C
   ↓
clang
   ↓
Native executable
```

This keeps early compiler engineering focused on **toro** rather than register allocation, instruction selection, object-file formats, and platform ABIs.

The frontend should avoid depending on C-specific semantics so that another backend can be introduced later.

Potential future backend:

```text
toro frontend
   ↓
toro IR
   ↓
LLVM backend
   ↓
x86-64 / ARM64 / ...
```

Do not switch merely because LLVM is more impressive. Switch when the existing backend materially limits toro's goals.

---

# Suggested Compiler Architecture

```text
src/
├── lexer/
│   ├── Lexer.cpp
│   └── Token.cpp
├── parser/
│   └── Parser.cpp
├── ast/
│   └── ...
├── semantic/
│   ├── Resolver.cpp
│   └── TypeChecker.cpp
├── types/
│   └── ...
├── codegen/
│   └── CBackend.cpp
├── diagnostics/
│   └── ...
├── runtime/
│   └── ...
└── cli/
    └── ...
```

Exact layout can evolve, but keep frontend, semantics, diagnostics, runtime, and backend concerns separated from the beginning.

---

# Milestone Philosophy

Each release should satisfy three questions:

### 1. Does the feature solve a real problem?

Do not add syntax solely because another language has it.

### 2. Can a programmer predict what it does?

If a basic feature requires a long explanation before ordinary use makes sense, simplify it.

### 3. Can toro explain its cost?

Especially for:

- Allocation
- ARC
- Copies
- Dynamic dispatch
- Closures
- Generic specialization
- Performance boundaries

toro should eventually make these costs inspectable rather than mysterious.

---

# Roadmap Summary

| Version | Primary milestone |
|---|---|
| **v0.1** | End-to-end native compiler; basic toro programs run |
| **v0.2** | Full function-call ergonomics and first-class functions |
| **v0.3** | ARC, weak references, construction, `destroy()` |
| **v0.4** | Inheritance, interfaces, virtual dispatch |
| **v0.5** | Generics, monomorphization, complete core collections |
| **v0.6** | Closures and expressive collection/data APIs |
| **v0.7** | Project tooling, formatter, tests, manifests, diagnostics |
| **v0.8** | Compiler-enforced `performance function` system |
| **v0.9** | C interop, packages, compatibility freeze, release candidate |
| **v1.0** | Stable language/toolchain suitable for public adoption |

---

# The First Goal

Before worrying about v0.2 through v1.0, toro has one job:

```bash
toro run main.toro
```

must successfully compile and execute a real toro program.

Everything else builds from there.
