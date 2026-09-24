# Toro Compiler and Runtime Architecture

## Current compiler pipeline

Toro currently uses a direct frontend-to-C architecture:

```text
.toro source
    ↓
SourceFile
    ↓
Lexer
    ↓
Tokens
    ↓
Parser
    ↓
AST
    ↓
SemanticAnalyzer
    ↓
TypeChecker
    ↓
CGenerator
    ↓
generated C11
    ↓
NativeCompiler
    ↓
clang (preferred) / cc
    ↓
native executable
```

The implementation compiler is C++23.

The CLI currently includes development/runtime commands such as:

```text
toro tokens
toro ast-expression
toro ast
toro check
toro emit-c
toro build
toro run
```

## Frontend

### Source and lexer

Source files use `.toro`.

The lexer tracks one-based line/column locations and recognizes canonical Toro
syntax, including `::` for type-scoped enum variants.

### Parser and AST

The parser owns syntax only. Semantic rules should not be moved into parsing merely for convenience.

The AST represents, among other features:

- primitive and named expressions;
- casts;
- calls and named arguments;
- member access;
- type-scoped enum access;
- statements/blocks/control flow;
- functions;
- structs/classes/interfaces/enums;
- inheritance/modifiers;
- generics;
- conversion overloads;
- lifecycle declarations;
- Result propagation.

### Semantic analysis

Semantic analysis handles scopes, declarations, name resolution, duplicate declarations, loop/handle bindings, and contextual validity such as `self`.

### Type checker

The type checker currently owns the static semantics for:

- primitive inference/checking;
- nullability;
- explicit `as` conversion;
- user conversion overloads;
- member and construction typing;
- visibility;
- inheritance and interfaces;
- overload resolution;
- generic inference and constraints;
- generic struct/class type instances;
- enums and exhaustive `handle`;
- `Result<T,E>`;
- `?` propagation;
- return-path analysis;
- class initialization and definite field initialization;
- weak-field restrictions;
- virtual/override semantics.

## Current C backend

The C backend is intentionally straightforward and correctness-first.

### Primitive lowering

Typical mappings:

```text
int     -> int64_t
dec     -> double
bool    -> bool
string  -> const char*
```

The backend lowers supported expressions, variables, functions, calls, control flow, and primitive printing.

### Structs

Non-generic structs lower to C structs with value semantics.

Supported behavior includes:

- named/positional construction;
- explicit and zero defaults;
- nested supported structs;
- field access/assignment;
- value copying;
- struct methods using an internal receiver.

### Enums

Toro enums support both traditional variants and data-carrying variants.

Enum variants use:

```toro
Message::text("hello")
Message::quit
```

The backend lowers data-carrying enums to deterministic tagged unions.

`handle` lowers to exhaustive C switch logic.

### Result

Concrete `Result<T,E>` instances lower to dedicated tagged-union C types with:

```text
ok(T)
error(E)
```

The backend supports:

- construction;
- parameters/returns;
- copies;
- exhaustive `handle`;
- postfix `?` propagation with single evaluation and early error return.

### Classes and ARC

Classes are heap-backed reference types with identity.

The current runtime implements:

- strong references by default;
- reference counting;
- strong field retain/release;
- deterministic final release;
- zeroing weak references through control blocks;
- `destroy()` execution;
- optional `init(...)`;
- definite initialization for required fields.

Lifecycle declarations use the dedicated source forms `init(...) { ... }` and
`destroy() { ... }`; they are not ordinary `function` methods.

Weak class fields use nullable class types and do not keep targets alive.

Strong cycles may leak; no tracing/cycle collector currently exists.

### Inheritance

Single class inheritance uses an embedded-base representation.

The immediate base object is the first member of the derived C object.

The hierarchy root owns shared runtime metadata such as:

- strong reference count;
- weak-control state;
- most-derived finalizer;
- hierarchy virtual-dispatch metadata.

Derived-to-base conversions preserve one allocation, one identity, one ARC lifetime, and one weak-reference state.

### Virtual dispatch

Virtual dispatch uses deterministic hierarchy vtables.

Concrete objects select a vtable at construction.

Generated thunks adjust from the stored most-derived object pointer to the receiver type expected by the selected implementation.

The design supports:

- base-reference dispatch to derived overrides;
- inherited virtual implementations;
- multi-level overrides;
- parameters and returned base references;
- virtual calls through `self`.

Non-virtual methods remain statically dispatched.

`destroy()` is not virtual; lifecycle cleanup remains deterministic derived-to-base.

### Runtime interfaces

Each interface lowers to a deterministic value struct and vtable. Class-backed
interface values store the existing class reference with retain/release callbacks;
they do not allocate a second object and therefore preserve ARC, weak-reference,
inheritance, and identity behavior. Struct-backed interface values store an
inline union member and copy with ordinary Toro value semantics.

Generated per-implementer thunks isolate dispatch adaptation from ordinary
expression lowering. Class thunks use the existing class vtable when an
interface requirement is implemented by a virtual method, so most-derived
overrides remain effective. Struct thunks call the concrete receiver directly.
Interface parameters, returns, locals, reassignment, inherited implementations,
and multiple interfaces use the same representation.

## Current known runtime boundaries

After the runtime-interface milestone, important unsupported or deferred runtime features include:

- generic function, struct, and class runtime lowering/monomorphization;
- generic interfaces and generic interface methods;
- interface-valued fields;
- function/method overload lowering where the backend does not yet support it;
- user-defined conversion overload lowering;
- RTTI/dynamic casts;
- multiple inheritance;
- base-initializer chaining;
- full collection runtime;
- modules/multi-file project compilation;
- C/C++ FFI;
- ownership/escape/borrow optimization;
- `performance function` enforcement.

The type system may understand some concepts before the C backend can execute them. The backend should reject unsupported constructs clearly rather than emit incorrect C.

## Near-term architecture principle

The existing C backend is a semantic proving ground.

Do not prematurely optimize ARC or allocation behavior inside the C generator merely to approximate the future ownership model.

Correct runtime behavior is the priority.

## Future toro IR

Before sophisticated ownership/performance optimization, Toro should introduce
an intermediate representation between static semantics and backend lowering.

Target pipeline:

```text
source
    ↓
frontend
    ↓
typed toro IR
    ↓
ownership analysis
    ↓
escape analysis
    ↓
borrow analysis
    ↓
lifetime/storage decisions
    ↓
ARC insertion where required
    ↓
ARC optimization/elision
    ↓
performance-contract validation
    ↓
backend
```

The IR should eventually be able to represent or annotate:

- value ownership;
- class/reference identity;
- ownership transfer;
- temporary borrowing;
- escaping values;
- stack/heap/arena eligibility;
- retain/release operations;
- destruction;
- dynamic dispatch;
- allocation;
- blocking/synchronization properties;
- performance-contract effects.

This must remain an internal compiler model. Normal Toro syntax should not
mirror every IR ownership concept.

## Ownership optimization direction

Future analyses should be able to prove cases such as:

### Borrow-only call

```toro
function print_user(user: User) {
    print(user.name)
}
```

If the callee cannot retain `user`, the compiler should eventually avoid unnecessary retain/release traffic.

### Ownership transfer

```toro
player := Player()
world.add(player)
```

If `player` is dead afterward and `world` becomes the owner, the compiler may transfer ownership without retain/release ping-pong.

### Stack allocation

```toro
function update() {
    enemy := Enemy()
    enemy.update()
}
```

If `enemy` does not escape or participate in an incompatible weak/lifetime relationship, it may eventually be stack allocated while retaining class/reference semantics at the source level.

## performance function architecture

`performance function` is planned as a compiler-enforced execution contract.

It is not merely an optimization level.

The compiler should eventually validate the transitive call graph and reject prohibited behavior such as:

- general heap allocation;
- blocking I/O;
- sleeping/waiting;
- hidden synchronization;
- escaping temporaries;
- allocating conversions;
- calls whose effects violate the contract.

Dynamic dispatch need not automatically be forbidden, but its cost/effect should be visible to analysis and inspection.

## Observability architecture

Future tooling should expose compiler decisions.

Possible surfaces include:

```text
toro inspect
toro inspect --memory
toro build --memory-report
toro explain-memory <file>:<line>
```

Reports may eventually include:

- stack/heap/arena allocations;
- ARC retains/releases;
- elided ARC operations;
- atomic ARC operations;
- escape reasons;
- ownership transfer;
- borrow decisions;
- potential cycles;
- dynamic dispatch;
- performance-contract violations.

The goal is automatic management without opaque behavior.

## Advanced future runtime work

Later phases may explore:

- arenas/scoped bulk allocation;
- local vs shared/thread-safe ARC;
- cycle diagnostics;
- stronger interprocedural ownership summaries;
- LLVM or another backend after the IR is mature;
- CMake integration for mixed native projects;
- first-class CMake language support only much later if justified.

These are roadmap items, not current implementation requirements.
