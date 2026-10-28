# Toro

Toro is a statically typed, native programming language focused on readable syntax, high performance, and progressive low-level control.

## Status

Toro is currently under active development.

Contributor design investigations live under `docs/`. The Phase 12.1 arena
feasibility study is recorded in `docs/ARENA_INVESTIGATION.md`. Its original
defer gate required representative runtime evidence; Application Milestone A2
has now met the cost-attribution criterion and reopens a design investigation.
Toro still has no arena syntax or runtime support, and no implementation has
been authorized.

The first representative application lives in `applications/simulation/`.
It is a deterministic multi-module simulation with persistent actors,
tick-local events/effects, virtual dispatch, collections, `Result`, weak
references, lifecycle behavior, and performance-contracted numeric helpers.
Its evidence report records current memory and ergonomics findings. The
follow-up external profiling study is recorded in
`applications/simulation/PROFILE_REPORT.md`. It found material bounded
tick-local allocation/lifecycle cost and reopens arena *design investigation*
without authorizing an arena implementation. It found no evidence sufficient
to reopen concurrency design.

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
Generic struct and class instances retain their concrete type arguments. The
checker validates explicit or inferred generic construction, constraints, and
substituted field and method types, including recursively nested generic types.
Enum variants are typed constructors, and `handle` validates case payload
bindings, variant identity, duplicate cases, and exhaustive coverage.
Enum variants use type-scoped access such as `Message::text("hello")` and
`Message::quit`; `.` remains exclusively instance/member access.
A native backend emits portable C11 for the currently supported primitive
procedural subset, concrete value structs, and enums with exhaustive
`handle` statements. Concrete `Result<T, E>` values, exhaustive Result handling,
and postfix `?` propagation are also lowered when both payload types are
backend-supported. Concrete classes lower to heap objects with identity and
generated strong-reference ARC operations. Class construction executes a single
`init` lifecycle declaration after allocating the object and establishing all
field defaults.
Single class inheritance uses one shared object identity and ARC lifetime.
Virtual methods dispatch through deterministic per-concrete-class vtables,
including calls through base references, parameters, returned references, and
`self`.
Concrete generic functions, structs, classes, and methods are monomorphized
with deterministic generated names. Their substituted fields, calls, returns,
ARC, inheritance, virtual dispatch, and interface conversions reuse the same
lowering as ordinary concrete declarations.
Concrete core collections also execute natively: `Array<T>` is a fixed-size
value with independent copies, while `List<T>` and `Map<string, V>` are shared
reference-backed containers with explicit `clone()` operations. Indexing,
mutation, counts, Array/List iteration, and class-element ARC are supported.
The Phase 6 backend baseline is hardened for deterministic generated C,
centralized runtime-type validation, consistent call-temporary ownership, and
clear unsupported-feature diagnostics. Phase 7 application work now builds on
that runtime baseline.
Module-root imports now allow one entry file to load a deterministic graph of
`.toro` modules. Top-level declarations are private by default and use `public`
for explicit export. Module-aware declaration identities allow independent
modules to reuse source-level names while preserving distinct types and native
symbols. Compiler-provided standard modules now supply console output,
text-file basics, environment access, process exit, wall-clock time, and
foundational string operations through normal Toro imports. Host interaction
remains isolated behind a private intrinsic boundary; application code sees
ordinary typed Toro APIs.
An explicit, narrow C FFI additionally supports bodyless `extern "C" function`
declarations using dedicated ABI scalar and borrowed-string types. This public
FFI remains separate from compiler-private standard-library intrinsics.
The checked frontend now lowers every program into a deterministic typed Toro IR
before C generation. The IR retains stable declaration choices, concrete generic
substitutions, dispatch kinds, runtime-value categories, and source origins. The
C backend consumes that IR without retaining or reading the originating AST;
current correctness-first ARC and runtime-helper behavior are preserved. A
canonical interprocedural ownership-summary pass records parameter-specific
observation, retain, store, return, escape, weak-store, and unknown behavior,
plus managed-return relationships. It propagates those facts through direct,
recursive, virtual, and interface calls with specialization-specific generic
identities. Escape analysis and borrow inference consume this shared contract;
foreign or unresolved calls remain conservative. A separate ARC materialization
pass records the lifetime schedule. A block-local
ownership-transfer pass recognizes final-use moves through locals, returns,
strong fields, and supported class-valued collections. Borrow inference then
records operation-bounded receiver, parameter, call-argument, and local-alias
observations and removes only the corresponding proven ARC pairs. `toro ir
--ownership` exposes callable contracts and causes; `toro ir --arc` exposes
transfers, borrows, and deterministic before/after metrics. Escaping stores,
weak access, uncertain control flow, and unknown/foreign ownership boundaries
remain conservative.
This is compiler-internal analysis and adds no source-level ownership syntax.
After ownership summaries, a deterministic strong-cycle pass inspects only
persistent owning storage. It reports advisory, source-located warnings for
possible self or multi-type ARC cycles across strong fields, inheritance,
concrete generics, supported collections, and persistent enum/Result payloads.
Weak fields and temporary/borrow/interface relationships do not form ownership
edges. Warnings neither reject the program nor rewrite ownership.
An allocation-site escape pass now follows effect analysis and classifies each
managed construction as non-escaping, escaping, or unknown. It propagates local
aliases, merges branches and loops conservatively, records compact reasons and
source origins, and treats weak observation as a storage constraint rather than
an owning escape. `toro ir --escape` exposes the facts and allocation counts.
The results are annotations only: objects still use the existing heap-backed ARC
representation unless the storage-decision pass can prove a direct local owner.
Proven local class constructions now use automatic C storage while retaining
Toro reference identity, inheritance and dispatch behavior, initialization, and
exactly-once lifecycle cleanup. Escaping, uncertain, weak-observed, reassigned,
or indirectly owned constructions remain heap-backed. The choice is shown by
`toro ir --escape`; no source-level allocation syntax is added.
Effect analysis remains descriptive rather than policy enforcement.
The backend-independent effect pass summarizes allocation, lifetime relevance,
I/O, native/foreign calls, dynamic dispatch, termination, and uncertainty across
the transitive call graph. These summaries are inspection metadata only; they do
not optimize code or enforce performance contracts. Phase 10.1 now accepts the
canonical `performance function` spelling and preserves the contract through
the formatter and typed IR. Phase 10.2 now applies a dedicated transitive
effect-policy pass: marked callables reject allocation, escaping values,
blocking/filesystem I/O, synchronization, and unknown external behavior while
ordinary functions remain unrestricted. Phase 10.3 now applies that policy to
the proven final runtime plan: exact class allocation sites selected for stack
storage no longer count as general heap allocation, while heap-backed sites,
collections, allocating runtime helpers, and genuine escapes remain invalid.
Effect summaries stay descriptive and unchanged; the contract pass consumes
storage, transfer, borrow, and optimized ARC facts after their canonical
analyses complete. `toro ir --performance` provides deterministic
contract-specific inspection: satisfied callables show material runtime proofs,
while violated and unproven callables show concise call paths, final effect
origins, dynamic targets, and available storage/escape reasons. Explanation is
separate from verdict calculation and does not change the Phase 10.3 policy.

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

Load and print a Toro source file:

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

Parse classes, visibility, and lifecycle declarations:

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

Run the focused generic-runtime example:

```bash
./build/toro run examples/generics.toro
./build/toro run examples/generic_runtime_probe.toro
./build/toro run examples/generic_interface_probe.toro
./build/toro run examples/generic_inheritance_probe.toro
```

Run the full semantic-analysis and type-checking pipeline on a source file:

```bash
./build/toro check examples/hello.toro
./build/toro check examples/modules/main.toro
```

Inspect the deterministic typed IR after semantic analysis and type checking:

```bash
./build/toro ir examples/backend_hardening.toro
./build/toro ir --effects examples/backend_hardening.toro
./build/toro ir --performance examples/performance.toro
./build/toro ir --ownership examples/backend_hardening.toro
./build/toro ir --escape examples/backend_hardening.toro
./build/toro ir --arc examples/backend_hardening.toro
./build/toro ir --arc --no-opt examples/backend_hardening.toro
```

Inspect the compiler's deterministic runtime plan overview:

```bash
./build/toro inspect examples/backend_hardening.toro
```

Request the detailed static memory report:

```bash
./build/toro inspect --memory examples/backend_hardening.toro
```

Explain the memory decision at one exact source location:

```bash
./build/toro explain-memory examples/class_arc.toro:15:5
```

`toro inspect` reports stable allocation identities and stack/heap decisions,
dynamic dispatch targets, optimized ARC before/after behavior, ownership
transfers and inferred borrows, capture count, and blocking/synchronization
effect sites. It consumes existing analysis results and does not alter generated
code. The memory report adds program and per-callable managed-allocation-site,
escape, ARC, transfer, borrow, weak-field, and possible-cycle detail. Its
numbers are static compiler sites and operations, not runtime execution counts
or profiling data. Project mode is available as `toro inspect` or
`toro inspect --memory` from a directory containing `toro.toml`.
`toro explain-memory <source.toro>:<line>[:<column>]` is the targeted static
view. If one line contains relevant facts at multiple columns, it lists those
columns deterministically and asks for a column. Locations without a memory
fact report that cleanly. The command consumes completed analyses and does not
change optimization or generated code. In project mode, the target file must
belong to the loaded project.

`--ownership` shows deterministic parameter behavior, compact transitive
causes, and managed-return relationships. `--escape` shows deterministic managed-allocation classifications, storage
decisions, fallback reasons, origins, and non-escaping/escaping/unknown plus
stack/heap metrics. `--arc` shows deterministic
before/after lifetime operations and retain,
release, and elided-pair counts. `--no-opt` exposes the materialized baseline.
These are compiler-development inspection formats, not stable source or ABI
contracts.

Like `check`, `emit-c`, `build`, and `run`, `ir` also supports project mode when
the current directory contains `toro.toml`.

Project commands may omit the source file when the current directory contains
`toro.toml`:

```bash
cd examples/project
../../build/toro check
../../build/toro emit-c
../../build/toro build
../../build/toro run
```

Format one source file in place, or every `.toro` file under a project's
configured source root:

```bash
./build/toro fmt examples/hello.toro
cd examples/testing
../../build/toro fmt
```

Formatting is deterministic and writes files by default. `--check` performs the
same parse-and-format pass without writing and exits nonzero when any file would
change:

```bash
./build/toro fmt --check examples/hello.toro
cd examples/testing
../../build/toro fmt --check
```

The formatter parses the complete file before replacing it. In project mode it
parses every discovered source before writing any source, so malformed syntax
cannot cause a partially formatted project. Toro comments are not yet part of
the lexer/AST; a file containing comment syntax is rejected and left unchanged
rather than having comment text discarded.

Run all project tests with:

```bash
cd examples/testing
../../build/toro test
```

Tests live beneath `<source.root>/tests` and are ordinary, non-generic Toro
functions named `test_*` that take no parameters and explicitly return `bool`.
Returning `true` passes and returning `false` fails. Files and functions are
discovered in deterministic relative-path/name order; each test is compiled
through the normal module, semantic, type-checking, C11, and native pipeline and
then executed independently. Project and standard-library imports work normally.
An invalid test signature, compile failure, false result, runtime failure, or a
project with no discovered tests makes `toro test` return nonzero. This boolean
return is the initial minimal assertion convention; dedicated assertion APIs and
source-level test declarations remain deferred.

The minimal manifest schema is:

```toml
[project]
name = "hello"

[source]
root = "src"
entry = "main.toro"
```

Projects that call symbols from system C libraries may add a structured linker
library list:

```toml
[ffi]
libraries = ["m"]
```

Entries are plain library names and become `-l<name>` arguments; paths, raw
flags, and shell commands are rejected.

The initial C FFI syntax and explicit ABI types are:

```toro
extern "C" function puts(text: c_string) -> c_int
extern "C" function pow(base: c_double, exponent: c_double) -> c_double

puts("Hello from C" as c_string)
result := pow(2.0 as c_double, 3.0 as c_double) as dec
```

Supported types are `c_int`, `c_int64`, `c_double`, `c_bool`, and non-null
borrowed `c_string`. Toro primitives cross the boundary only through explicit
`as` casts. C strings are immutable and non-owning; C must not mutate or free a
Toro-derived pointer. Raw pointers, nullable C pointers/strings, variadics,
callbacks, C aggregates, automatic header parsing, and C++ ABI calls remain
unsupported. See `LANGUAGE.md` for the complete lifetime rules.

Discovery checks exactly `./toro.toml`; it does not search parent directories.
`source.root` is relative to the manifest directory, and `source.entry` is
relative to that source root. Both must remain within the project, and the entry
must name an existing `.toro` file. In project mode, imports resolve from the
configured source root. Direct-file invocations continue to use the entry
file's directory as their source root.

Generate C after parsing and checking a source file:

```bash
./build/toro emit-c examples/hello.toro
./build/toro emit-c examples/structs.toro
./build/toro emit-c examples/enums.toro
./build/toro emit-c examples/results.toro
./build/toro emit-c examples/class_arc.toro
./build/toro emit-c examples/lifecycle.toro
./build/toro emit-c examples/init.toro
./build/toro emit-c examples/class_inheritance.toro
./build/toro emit-c examples/virtual_dispatch.toro
./build/toro emit-c examples/interfaces.toro
```

Build a native executable with the host C compiler:

```bash
./build/toro build examples/hello.toro
./build/toro build examples/hello.toro -o /tmp/toro-hello
```

Without `-o`, the executable is written to the current directory using the
source filename stem, such as `./hello`. In project mode it uses the manifest's
project name. The native toolchain prefers `clang` and falls back to `cc`,
compiling the generated source as C11.

Build and run through temporary artifacts:

```bash
./build/toro run examples/hello.toro
./build/toro run examples/structs.toro
./build/toro run examples/enums.toro
./build/toro run examples/results.toro
./build/toro run examples/class_arc.toro
./build/toro run examples/lifecycle.toro
./build/toro run examples/init.toro
./build/toro run examples/class_inheritance.toro
./build/toro run examples/virtual_dispatch.toro
./build/toro run examples/interfaces.toro
./build/toro run examples/overloads.toro
./build/toro run examples/conversions.toro
./build/toro run examples/collections.toro
./build/toro run examples/backend_hardening.toro
./build/toro run examples/modules/main.toro
```

Run the standard-library project example:

```bash
cd examples/stdlib
../../build/toro check
../../build/toro emit-c
../../build/toro run
```

Run the C FFI project example:

```bash
cd examples/ffi
../../build/toro check
../../build/toro emit-c
../../build/toro build
../../build/toro run
```

### CMake integration

`cmake/ToroIntegration.cmake` lets an existing C/C++ CMake build compile a
normal manifest-backed Toro project into a static library target:

```cmake
list(APPEND CMAKE_MODULE_PATH "/path/to/toro/cmake")
include(ToroIntegration)

toro_add_target(
    NAME game_logic
    PROJECT "${CMAKE_CURRENT_SOURCE_DIR}/toro"
)

add_executable(game cpp/native.cpp)
target_link_libraries(game PRIVATE game_logic)
```

The consuming project must enable C. The helper uses `TORO_EXECUTABLE` when it
is set and otherwise locates `toro` with `find_program`. It asks the compiler to
inspect `toro.toml`, generates C with `toro emit-c -o`, builds a deterministic
`game_logic` static-library artifact, and propagates manifest `[ffi].libraries`
to consumers. Toro sources and `toro.toml` are build dependencies; CMake also
discovers added `.toro` files beneath the configured source root.

The current link direction uses Toro's generated `main`: native C/C++ objects
linked into the executable may provide explicitly declared `extern "C"`
functions that Toro calls. Native code cannot directly call ordinary Toro
functions because exported Toro entry points, callbacks, and header generation
are not yet supported. The native executable must not define another `main`.
See `examples/cmake_integration` for a complete C++/Toro build.

For build-system integrations, `toro project-info --cmake` emits validated
project metadata as CMake assignments. This is a machine-facing interface; it
keeps manifest parsing in Toro rather than duplicating it in CMake.

Imports use dotted module identities:

```toro
import game.player
import util.math
```

For direct-file commands, the entry file's directory is the source root. For
project commands, the manifest's `[source].root` selects it. In either mode,
`game.player` maps to `game/player.toro` beneath that root. Imported top-level
declarations must be marked `public`; unmarked declarations remain private to
their module. Duplicate and diamond
imports are deduplicated, while missing modules, self-imports, and cycles are
diagnosed. Aliases, wildcards, external packages, dependencies, and additional
configured source roots remain deferred.
Same-named declarations in separate modules remain distinct. An unqualified use
is diagnosed as ambiguous when two directly imported modules export that name;
aliases and module-qualified source access remain deferred.

The reserved standard-library modules `io`, `fs`, `env`, `process`, `time`, and
`strings` resolve before project modules of the same name. They use stable
compiler-provided identities, so generated output is independent of installation
and project paths. The initial surface includes `write_line`, text-file
`exists`/`read_text`/`write_text`/`remove_file`, environment `has`/`get`,
process `exit`, wall/elapsed millisecond time, and string length,
concatenation, search, prefix/suffix, and checked substring operations. See
`LANGUAGE.md` for complete signatures and failure behavior.

`run` forwards program output and returns its exit status. Its temporary C source
and executable are removed after the program finishes or compilation fails.

The `check` command reports name-resolution errors, type mismatches, invalid
conditions and returns, bad construction fields, invalid member access, method
argument errors, and visibility violations. Construction supplies zero values
for primitive and nullable fields, honors explicit field defaults, and requires
non-null named fields that cannot be safely defaulted. Generic instances are
invariant, so different concrete argument lists are distinct types.
Flow-sensitive null narrowing is not implemented yet.
Functions and methods may overload by parameter types and
arity; resolution prefers exact matches over base/interface compatibility and
reports missing or ambiguous matches. Explicit `as` casts support numeric
conversions and user-defined class or struct conversions. Generic function calls
infer type arguments, accept validated explicit type arguments, substitute return
types, and enforce interface constraints. Inference recursively matches nested
types such as `List<T>` and `Map<string, List<T>>`.
Enum construction and exhaustive `handle` checking are also part of this pass.
`ok(value)` and `error(value)` construct an expected `Result<T, E>`, while
postfix `?` extracts the success type and propagates a compatible error from a
function returning `Result`. The C backend gives each concrete Result instance a
distinct tagged-union type. Propagation evaluates its operand once and returns a
new error Result immediately when needed.
Functions and concrete methods with declared return types must return on every
reachable path. Complete `if`/`else` trees and exhaustive returning `handle`
statements satisfy this requirement; loops are not assumed to execute.

The initial C backend lowers primitive variables, expressions, functions,
returns, calls, `if`/`else`, `while`, primitive `print(...)` calls, and
concrete structs. Struct construction emits every field explicitly, applying
source defaults or Toro primitive zero values. Field access, chained access,
assignment, value copies, and struct methods use native C value semantics;
methods lower to prefixed functions with an internal receiver pointer. It uses
deterministically prefixed C identifiers and emits a C entry-point wrapper for a
Toro `main`. Enums lower to deterministic tagged unions; construction selects a
tag and payload, while exhaustive `handle` statements lower to `switch` blocks
with case-scoped payload bindings. Concrete generic functions and types are
monomorphized on demand. Unsupported nullable value fields, Results containing
unsupported runtime payloads, and
other runtime types outside this subset fail with a backend diagnostic instead
of producing partial or incorrect C. Classes use heap-backed pointer
identity. Construction begins with one strong reference; local copies and class
parameters retain, replacement and lexical-scope exit release, and the final
release frees the object. Primitive and supported value fields, field access and
assignment, methods, `self`, and class-valued function parameters/returns are
lowered. Strong class-reference fields retain their targets and release replaced
or owned values. Nullable class fields accept `null`. A class `destroy()`
lifecycle declaration runs once after its weak control block is invalidated but
before its strong and weak fields are cleaned and object storage is freed. Weak
fields must have a nullable class type; they use zeroing control-block references, do not retain
their target, and safely read as `null` after target destruction. Parent-strong
to child / child-weak to parent graphs therefore clean up normally. Strong
reference cycles can still leak because Toro does not yet include a cycle
collector. The compiler warns about statically meaningful possible strong
cycles and suggests considering whether a non-owning back-reference should be
`weak`; this remains advisory, and runtime behavior is unchanged. When a class
declares `init`, constructor arguments bind to its
parameters instead of fields. The backend initializes explicit defaults, Toro
zero values, and nullable fields before invoking `init` exactly once. Required
non-null fields without defaults must be assigned by `init` on every reachable
completion path. Classes without `init` retain field-based construction.

Core collections lower to deterministic C specializations and helper functions.
`Array<T>` owns fixed contiguous storage and copies independently; `List<T>` and
`Map<string, V>` use shared non-atomic reference-counted backing storage and
provide `clone()` for independent storage. Array/List indexing is bounds checked,
`for ... in` iterates them in index order, and stored class references are
retained and released with the collection. Map iteration and non-string Map keys
remain deferred pending canonical hashing and entry-binding semantics. Strings
continue to use the existing immutable `const char*` lowering because no owned
string operation is required by the current collection API.

Initializers and destructors use the dedicated `init(...) { ... }` and
`destroy() { ... }` lifecycle syntax; `function init(...)` and
`function destroy()` are rejected.
Derived objects use a prefix-compatible embedded-base layout containing one
ARC/weak/finalizer header. Derived-to-base upcasts therefore preserve object
identity, and a base-typed final release invokes the most-derived finalizer.
Destruction runs derived lifecycle cleanup before base lifecycle cleanup, then
releases all derived and inherited strong/weak fields exactly once. Inherited
non-virtual methods use static calls with a safe base-pointer view. Hierarchies
with virtual methods store deterministic dispatch metadata in the shared root
header. Concrete-class vtables use receiver-adjusting thunks, so calls through
derived or base references, parameters, returns, and `self` select the
most-derived override. Abstract bodyless virtual methods require a concrete
implementation. `destroy()` stays outside virtual dispatch and follows the
deterministic derived-to-base lifecycle path.
Interface values use deterministic interface vtables. Class-backed values keep
the existing object reference and participate in the same ARC/weak lifetime;
struct-backed values keep an inline copy and preserve value semantics. Native
interface conversion, calls, parameters, returns, reassignment, multiple
interfaces, inherited implementations, and virtual overrides are supported.
Resolved free-function, struct-method, class-method, and interface-method
overloads lower to deterministic signature-derived symbols; generic fallback
specializations retain both overload and concrete-type identity. Explicit
`int`/`dec` casts and class/struct `overload as Type` bodies execute natively.
Initializer overloading, base-initializer chaining, generic interfaces and
interface methods, interface-valued fields, class-reference struct fields, class-valued
enum/Result payloads, and field access through a
temporary class reference remain explicit backend errors.

Generated declaration and helper ordering follows deterministic discovery
registries rather than unordered lookup-map iteration. Concrete specialization
names include signature and recursive type identity, with collision checks at
backend registration. Repeated generation is tested for byte-identical C, and
representative output is validated as strict C11. The focused
`backend_hardening.toro` example combines the major Phase 6 runtime features in
one native program.

## Logical operators

Toro uses the keywords `and` and `or` for logical expressions. `and` binds more
tightly than `or`, while equality and comparison operators bind more tightly than
both. These operators short-circuit during native execution:

```text
false and expression  // expression not evaluated
true or expression    // expression not evaluated
```

The symbolic forms `&&` and `||` are not part of Toro.

Run the test suite:

```bash
ctest --test-dir build --output-on-failure
```

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
```
