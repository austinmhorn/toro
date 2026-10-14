# Toro Roadmap

This roadmap reflects the current language direction after completion of the
core semantic system and the completed Phase 6 native runtime baseline.

Toro's long-term thesis is:

> **A performance-scalable native application language: ergonomic normal code, compiler-driven ownership/lifetime optimization, enforceable performance contracts for hot paths, and tooling that explains what the compiler decided.**

The guiding memory principle is:

> **Ownership analysis, not ownership programming.**

## Current baseline

Completed foundations include:

- C++23 compiler skeleton and `.toro` source loading;
- lexer, parser, AST, diagnostics;
- variables, functions, control flow, loops;
- enums and exhaustive `handle`;
- structs/classes/interfaces;
- single inheritance, abstract/virtual/override semantics;
- generics and constraints;
- semantic analysis and name resolution;
- primitive typing/inference and nullability;
- explicit `as` casts;
- user-defined `overload as Type`;
- member/construction typing and visibility;
- overload resolution;
- generic function/type inference;
- `Result<T,E>` with `ok`, `error`, and postfix `?`;
- return-path analysis;
- C11 code generation;
- native `build` / `run`;
- struct runtime;
- tagged-union enum runtime;
- Result runtime;
- class ARC;
- zeroing weak references;
- `init` / `destroy`;
- single-inheritance runtime;
- virtual dispatch runtime;
- runtime interface values and dispatch for classes and structs;
- a deterministic typed Toro IR boundary before C generation;
- deterministic local/transitive IR effect summaries.

The current implementation prioritizes correct semantics over advanced optimization.

---

# Phase 6 — Complete the native runtime

## 6.11 Runtime interfaces — completed

Interface values and dispatch now execute natively.

Goals:

- class/struct to interface conversion;
- interface method dispatch;
- multiple implemented interfaces;
- identity/lifetime correctness for class-backed interface values;
- value semantics for struct-backed interface values where appropriate;
- clear representation and dispatch strategy;
- preserve existing ARC/inheritance behavior.

## 6.12 Generic runtime lowering — completed

Lower concrete generic instantiations.

Goals:

- monomorphized generic functions;
- generic structs;
- generic classes;
- substituted fields/methods;
- generic constraints honored by the backend;
- deterministic generated names;
- no runtime type-erasure unless deliberately introduced later.

The backend now specializes reachable concrete generic functions, structs,
classes, and methods. Substitution flows through nested types, members, calls,
and returns, while generic classes reuse the existing ARC, inheritance, virtual,
interface, and lifecycle machinery.

## 6.13 Remaining overload/conversion runtime support — completed

Complete runtime lowering for semantics already understood by the frontend.

Completed behavior:

- free-function, method, virtual-method, and interface-method overload sets;
- declaration-identity handoff from type checking to code generation;
- deterministic signature-derived native symbols;
- built-in numeric casts and user-defined `overload as Type`;
- interaction with inheritance, interfaces, and concrete generic instantiations;
- preserved class ARC and struct value semantics during conversions.

## 6.14 Core collections and strings — completed

Implement the first real standard runtime containers.

Target concepts:

- `Array<T>` fixed-size contiguous values;
- `List<T>` growable contiguous storage;
- `Map<K,V>` hash map;
- stronger owned string runtime if required beyond current C-string lowering;
- zero-based indexing;
- default bounds checking;
- iteration support;
- `List`/`Map` shared/reference-backed semantics;
- explicit `.clone()` for independent copies.

Keep container ownership semantics consistent with the language memory model.

Completed behavior:

- fixed contiguous `Array<T>` values with independent copies, indexing,
  mutation, count/length, iteration, pass/return behavior, and class-element ARC;
- shared growable `List<T>` storage with add/append, indexing, iteration, and
  independent `.clone()` storage;
- shared hash-backed `Map<string, V>` storage with insert/update, lookup,
  presence tests, count/length, and independent `.clone()` storage;
- deterministic concrete C11 collection specializations and reusable helpers;
- retained immutable `const char*` strings because current operations do not
  require an owned string representation.

Map iteration and additional key hashing/equality definitions remain deferred
until their source-level semantics are specified.

## 6.15 Runtime/backend hardening — completed

Before moving to larger application features:

- reduce duplicated C-generator logic;
- isolate backend/runtime helpers;
- harden diagnostics;
- improve generated-C determinism;
- add larger end-to-end native tests;
- document supported/unsupported backend boundaries.

Do not perform speculative ownership optimization here.

Completed behavior:

- centralized concrete collection/Result registration and backend validation;
- shared call-argument ownership preparation and parameter ARC setup;
- deterministic vector-backed emission registries with repeated-generation
  regression coverage;
- collision diagnostics for concrete generic specialization identities;
- strict C11 syntax validation for representative generated output;
- a larger native integration program covering classes, inheritance,
  interfaces, generics, overloads, conversions, collections, Result, enums,
  virtual dispatch, ARC, and lifecycle cleanup;
- regression coverage for Array value semantics through class initializers.

Phase 6 is complete. The direct C11 backend remains correctness-first and keeps
its documented unsupported boundaries explicit; Phase 7 may now build
application features on this runtime baseline.

---

# Phase 7 — Make Toro usable for real applications

The goal of this phase is to build non-trivial applications before optimizing hypothetical workloads.

## 7.1 Modules and imports — completed

Implement stable module-root imports:

```toro
import io
import net.http
import game.player
```

Avoid fragile relative-header-style semantics.

Completed behavior:

- module-scope `import` declarations with dotted module-root identities;
- deterministic entry-directory source-root resolution;
- private-by-default top-level declarations and explicit `public` exports;
- direct-import visibility without accidental transitive re-export;
- duplicate-import deduplication, self-import diagnostics, missing-module
  diagnostics, and cycle detection;
- deterministic dependency-first flattening into the existing checked backend
  program, with shared diamond imports emitted once;
- `check`, `emit-c`, `build`, and `run` support for imported module graphs;
- native multi-file example and focused frontend/runtime regression coverage.

The completed manifest milestone now supplies one configured source root.
Additional source roots, package resolution, aliases, and wildcard imports
remain deferred.

## 7.2 Module-aware multi-file compilation — completed

Expand the initial entry-root module graph into module-aware compilation units
that feed one native program.

Completed behavior:

- stable logical-module identities for nominal types, functions, overloads,
  generics, and concrete specializations;
- retained module boundaries and declaration ownership in the combined checked
  program;
- private-by-default per-module lookup with direct public imports and no
  accidental re-export;
- clear ambiguity diagnostics for colliding unqualified imports;
- collision-free deterministic native names without absolute filesystem paths;
- cross-module generics, inheritance, interfaces, enums, collections, Result,
  ARC, and lifecycle behavior;
- focused same-name, diamond, deterministic-emission, and native integration
  coverage.

Aliases, qualified module access, packages, and separate object-file compilation
remain deferred.

## 7.3 Project manifest — completed

Introduce a minimal Toro project manifest without package-manager behavior.

Completed behavior:

- `toro.toml` with required project name, source root, and entry source;
- exact-current-directory discovery with no surprising parent traversal;
- validated project/source/entry boundaries and focused configuration
  diagnostics;
- project-root `check`, `emit-c`, `build`, and `run` commands without a source
  argument;
- configured source-root integration with deterministic logical module
  identities;
- preserved explicit-file workflows;
- deterministic strict-C11 output and a native project example.

Dependencies, lockfiles, registries, workspaces, aliases, and additional source
roots remain deferred.

## 7.4 Standard library foundation — completed

Establish an importable application-development baseline without exposing a
general native ABI.

Completed behavior:

- compiler-provided `io`, `fs`, `env`, `process`, `time`, and `strings` modules;
- deterministic standard-library identities independent of project and install
  paths, with reserved-module precedence over project source roots;
- ordinary public Toro wrappers over one isolated private runtime-intrinsic
  registry;
- console output, text-file existence/read/write/removal, environment lookup,
  process exit, wall/elapsed milliseconds, and foundational string operations;
- `Result`-based filesystem, environment, and substring failures;
- process-lifetime immutable storage for native-produced strings, with cleanup
  at process shutdown;
- strict-C11, deterministic-emission, project-mode, and native integration
  coverage.

Console input, argument vectors, monotonic duration APIs, directory operations,
networking, regular expressions, structured serialization, richer owned-string
runtime semantics remain deferred. Public C interoperability is provided by the
narrow Phase 7.6 ABI boundary below.

## 7.5 Testing and formatting — completed

Completed behavior:

- `toro fmt <source.toro>` and manifest-aware project formatting with
  deterministic, idempotent AST-based output;
- non-mutating `toro fmt --check` for direct files and projects;
- parse-before-write protection so syntax failures never partially format a
  project;
- first-class `toro test` with deterministic discovery beneath
  `<source.root>/tests`;
- a minimal ordinary-function convention: non-generic, zero-argument
  `test_*() -> bool` functions pass by returning `true`;
- isolated normal-pipeline compilation and native execution for each test,
  including project and standard-library imports;
- concise diagnostics and nonzero status for invalid signatures, compile
  failures, false results, runtime failures, and missing tests;
- focused formatter/test-runner coverage and a project example.

Comment formatting remains deferred until comments are represented by the
lexer/AST; current formatting rejects such input without modifying it. Dedicated
assertion APIs, test attributes, setup/teardown hooks, coverage, and editor/LSP
diagnostic protocols also remain deferred.

## 7.6 C FFI — completed

Completed behavior:

- explicit bodyless `extern "C" function` declarations integrated with normal
  modules, visibility, name resolution, overload selection, and type checking;
- distinct `c_int`, `c_int64`, `c_double`, `c_bool`, and borrowed non-null
  `c_string` ABI types;
- explicit Toro/ABI scalar conversions and one-way `string as c_string` views;
- deterministic unmangled external prototypes and declaration-identity-driven
  calls;
- exact-once, left-to-right argument evaluation before entering C;
- structured `[ffi].libraries` manifest configuration translated to safe
  `-l<name>` process arguments for `build`, `run`, and project tests;
- module, direct-file, deterministic-emission, strict-C11, native libc/libm,
  diagnostics, and project-linking coverage.

Raw pointers, C aggregates, mutable/owned C strings, nullable C pointers,
callbacks, function pointers, variadics, automatic header parsing/bindgen,
dynamic loading, and non-C ABIs remain deferred.

## 7.7 Mixed native build integration — completed

Completed behavior:

- reusable `toro_add_target(NAME ... PROJECT ...)` support through
  `cmake/ToroIntegration.cmake`;
- conventional `find_program` discovery with an injectable `TORO_EXECUTABLE`;
- compiler-owned `toro project-info --cmake` manifest metadata, avoiding a
  second manifest parser in CMake;
- deterministic generated C and a stable static-library CMake target;
- transitive propagation of structured `[ffi].libraries` dependencies;
- source, manifest, compiler, clean-build, path-with-spaces, and unchanged-build
  dependency coverage;
- a mixed C++/Toro example in which Toro owns `main` and calls an explicitly
  declared C ABI function supplied by C++.

Direct C/C++ calls into ordinary Toro functions, exported bindings, callbacks,
header generation, per-module native objects, and first-class
`project(... LANGUAGES toro)` support remain deferred.

---

# Phase 8 — Toro IR and compiler analyses

This is the architectural pivot required for the long-term memory/performance thesis.

## 8.1 Typed IR foundation — completed

Add an intermediate representation between type checking and backend lowering.

Represent:

- typed values;
- calls;
- branches;
- allocations;
- object identity;
- destruction;
- dispatch;
- ownership-relevant operations/effects.

Initially lower IR to the existing C backend without changing observable semantics.

Completed behavior:

- a dedicated `IRLowerer` between type checking and C generation;
- deterministic typed declarations and operation trees with module/source
  origins, stable declaration identities, and generic substitutions;
- representation of procedural control flow, foreign/ordinary calls,
  construction, aggregates, classes, enums, Result, collections, conversions,
  `handle`, propagation, and virtual/interface dispatch;
- explicit runtime-value categories and a legacy-ARC lifetime-model marker;
- `toro ir` for deterministic developer inspection;
- C generation routed through `IRProgram` with unchanged generated C/native
  behavior and regression coverage.

Phase 8.1 originally used a temporary non-owning typed-AST compatibility
payload while the mature C emitter migrated. Phase 8.3 removed that payload;
the historical note remains relevant only to the staged implementation order.

## 8.2 Effect metadata — completed

Allow functions/calls to describe compiler-relevant behavior such as:

- may allocate;
- may block;
- may synchronize;
- may retain/escape references;
- dynamic dispatch;
- destruction effects.

This metadata becomes useful for both ownership optimization and future performance contracts.

Completed behavior:

- a dedicated backend-independent `EffectAnalyzer` over typed IR;
- deterministic local and transitive callable summaries computed to a monotonic
  call-graph fixed point, including terminating recursive and mutually recursive
  graphs;
- allocation, retain, release/destruction, escape, blocking, filesystem,
  environment, time, process I/O, synchronization, foreign/native call, dynamic
  dispatch, termination, and unknown-external effect categories;
- conservative foreign-call summaries and known metadata for compiler-private
  standard-library intrinsics;
- virtual/interface calls summarized from all matching known implementations,
  with explicit uncertainty when no implementation can be proven;
- concrete generic callable summaries retaining deterministic specialization
  identities;
- compact local/transitive causes with source origins and direct callee identity;
- deterministic `toro ir --effects` developer inspection.

Effect metadata describes what code may do. It does not optimize ARC, infer
borrows/moves/escapes, change allocation, or enforce `performance function`.

## 8.3 Backend decoupling — completed

Move semantics currently encoded directly in `CGenerator` behind IR/backend lowering.

The C backend remains useful as a portable reference backend.

LLVM or another backend should not be considered necessary until the IR is useful independently.

Completed behavior:

- `IRProgram` is the C backend's complete semantic input and no longer retains
  or references a typed AST;
- an IR-derived, backend-private emission model normalizes operations for the
  established C11 emitter without repeating frontend name, overload, generic,
  or type resolution;
- concrete specializations are discovered from IR declaration identities and
  substitutions, preserving module and nullable-type identity;
- runtime helpers are registered from IR-derived runtime operations and types;
- current correctness-first ARC is reproduced from typed construction,
  declaration, assignment, call, return, field, collection, and lifecycle
  operations under the `legacy-arc` lifetime model;
- the frontend convenience entry point is isolated from the IR-only backend and
  lowers through `IRLowerer` before C generation;
- detached `IRProgram` regression coverage proves that C generation does not
  require the originating AST to remain alive.

Phase 8.3 intentionally does not add ownership inference or optimize the
existing ARC schedule. Explicit ownership-transfer and borrow semantics remain
Phase 9 analysis work.

---

# Phase 9 — Ownership and lifetime optimization

This phase implements the principle:

> **Ownership analysis, not ownership programming.**

Normal Toro source should remain largely unchanged.

## 9.1 ARC retain/release elision

Remove trivially unnecessary retain/release pairs.

Establish correctness tests comparing optimized and unoptimized lifetime behavior.

## 9.2 Ownership transfer / move analysis

Recognize when a strong reference can transfer ownership without retain/release ping-pong.

Avoid mandatory source-level `move` syntax for obvious cases.

## 9.3 Escape analysis

Determine whether class instances escape their local lifetime.

Track escapes through:

- returns;
- fields;
- globals;
- collections;
- closures;
- weak relationships;
- unknown calls.

## 9.4 Stack allocation of non-escaping class instances

Allow class/reference semantics to use stack storage when the compiler proves it safe.

Source semantics must remain unchanged.

## 9.5 Borrow inference

Represent temporary non-owning access internally.

Eliminate ARC traffic for calls proven not to retain/escape references.

Do not expose lifetime syntax unless real programs demonstrate a need.

## 9.6 Interprocedural ownership summaries

Summarize function behavior:

- borrows;
- consumes/transfers;
- retains;
- returns ownership;
- causes escape.

Use summaries to improve analysis across call boundaries.

## 9.7 Strong-cycle diagnostics

Detect obvious or likely ARC cycles where practical.

Provide actionable diagnostics suggesting `weak` relationships.

Do not introduce a tracing collector merely to avoid designing ownership relationships.

---

# Phase 10 — `performance function`

Implement `performance function` as a compiler-enforced execution contract.

Example:

```toro
performance function update(world: World, dt: dec) {
    ...
}
```

## 10.1 Syntax/semantic activation

Promote the existing language design to executable compiler semantics.

## 10.2 Transitive effect checking

Validate the complete reachable call tree.

Initial prohibited effects should include, where practical:

- general heap allocation;
- blocking I/O;
- sleep/wait;
- hidden synchronization;
- escaping temporaries;
- hidden allocating conversions;
- calls with unknown/prohibited effects.

## 10.3 Ownership-aware contract satisfaction

Allow ownership optimization to prove that code satisfies the contract.

Example:

A class instance that would normally use heap + ARC may still be legal if escape analysis proves it can be stack allocated with no ARC.

## 10.4 Diagnostics

Contract failures must explain why:

```text
performance contract violation

enemy requires heap allocation

reason:
  reference escapes through World.entities
```

Do not market `performance function` as a guarantee of algorithmic speed. It guarantees categories of execution behavior.

---

# Phase 11 — Memory and performance observability

Automation should be explainable.

## 11.1 `toro inspect`

Expose compiler-visible runtime properties such as:

- allocations;
- dynamic dispatch;
- ARC behavior;
- captures;
- blocking/synchronization effects.

## 11.2 Memory reports

Potential interface:

```text
toro build --memory-report
toro inspect --memory
```

Potential metrics:

- stack allocations;
- heap allocations;
- future arena allocations;
- ARC retains/releases;
- elided ARC operations;
- potential cycles;
- atomic ARC operations once concurrency exists.

## 11.3 `toro explain-memory`

Explain a specific source location:

```text
toro explain-memory src/game.toro:142
```

Possible output:

```text
enemy: Enemy

storage:
  heap

reason:
  escapes through World.entities

ownership:
  transferred to World

ARC:
  local
```

The goal is automatic memory management without opaque behavior.

---

# Phase 12 — Advanced memory and concurrency

Only implement these after real Toro applications and profiling justify them.

## 12.1 Arenas / scoped bulk allocation

Explore explicit arena/scoped allocation for workloads such as:

- games/simulation frames;
- parsers;
- request processing;
- compilers;
- media pipelines.

Exact syntax should be designed from real use cases, not prematurely.

## 12.2 Concurrency model

Define Toro concurrency before optimizing for it.

The language should eventually distinguish thread-local from thread-crossing ownership when safe.

## 12.3 Local vs shared ARC

Normal references should not automatically pay atomic reference-count costs.

Potential model:

```text
local reference
    ↓
non-atomic ARC

crosses concurrency boundary
    ↓
shared/thread-safe ARC
```

This depends on a real concurrency model and must not be guessed ahead of it.

## 12.4 Advanced ownership controls

Only if real programs demonstrate a need, consider explicit advanced controls for ambiguous ownership, borrowing, pinning, arenas, or low-level pointer/FFI behavior.

Do not make such syntax dominate ordinary Toro.

---

# Pre-1.0 validation

Toro should not declare success merely because the language can compile itself or run examples.

Before 1.0, build representative real applications in target domains.

Suggested validation projects:

- CLI utility;
- network/service application;
- parser/compiler-style tool;
- small native desktop application;
- game/simulation workload with meaningful hot loops.

Measure against relevant established languages.

Evaluate:

- runtime performance;
- allocation behavior;
- latency predictability;
- startup/binary characteristics;
- amount of programmer-visible ownership code;
- source complexity/readability;
- build/tooling experience;
- quality of diagnostics;
- effectiveness of `performance function`;
- usefulness/accuracy of inspection tooling.

The central question is:

> **Can Toro make native software meaningfully easier to write without giving up the ability to demand and understand low-level performance?**

A positive answer to that question is the strongest justification for Toro 1.0.
