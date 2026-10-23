# Toro Compiler and Runtime Architecture

## Current compiler pipeline

Toro now has a typed IR boundary between the checked frontend and the C backend:

```text
entry .toro source
    ↓
ProjectConfigLoader (project mode)
    ↓
ModuleLoader / source-root resolver
    ↓
ordered module graph
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
IRLowerer
    ↓
typed toro IR
    ↓
EffectAnalyzer
    ↓
annotated toro IR
    ↓
OwnershipSummaryAnalyzer
    ↓
StrongCycleAnalyzer
    ↓
EscapeAnalyzer
    ↓
StorageDecisionAnalyzer
    ↓
ArcMaterializer
    ↓
OwnershipTransferAnalyzer
    ↓
BorrowInferenceAnalyzer
    ↓
ArcOptimizer
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
toro fmt
toro test
```

`Formatter` is a frontend-only AST printer. It lexes and parses complete source
files, then emits deterministic canonical syntax without depending on semantic
analysis, type checking, or the C backend. Direct mode formats one file; project
mode discovers `.toro` files beneath the configured source root in stable path
order. A batch is fully parsed before any replacement, and check mode performs
no writes. Comments are intentionally rejected while the lexer/AST cannot retain
them safely.

`TestRunner` discovers ordinary `test_*() -> bool` functions beneath the
project source root's `tests` directory in stable path/name order. Each test is
loaded as its own entry module, receives a synthetic internal native entry point,
and otherwise travels through the normal module loader, semantic analyzer, type
checker, C generator, and native compiler. Separate native execution isolates a
false return or process failure to its test. The convention adds no test-only
syntax and does not couple testing to backend internals.

## Frontend

### Source and lexer

Source files use `.toro`.

`ProjectConfigLoader` owns `toro.toml` discovery and validation. Discovery checks
the current directory only. It resolves the project name, project root,
configured source root, and entry source before compilation; manifest parsing
does not leak into the CLI pipeline, module resolver, or frontend.

`ModuleLoader` owns module discovery after receiving an entry and source root.
Reserved standard-library identities resolve first from compiler-provided Toro
source, while application modules remain filesystem-backed. Direct-file
compilation supplies the entry file's directory;
project compilation supplies `[source].root`. A dotted identity such as
`game.player` maps to `game/player.toro` beneath that root. The loader performs a
deterministic depth-first traversal, reports missing/self/cyclic imports,
deduplicates repeated and diamond dependencies, and produces dependency-first
module metadata plus one combined owned AST program. Each module records its
statement range, imports, stable logical identity, and declaration ownership, so
the combined storage does not erase compilation-unit boundaries. Filesystem
resolution does not live in the parser, semantic analyzer, type checker, or C
backend.

The `StandardLibrary` registry owns stable module source and the small set of
private runtime-intrinsic identities used by those modules. Standard-library
wrappers still pass through normal parsing, public-import visibility, type
checking, declaration identity, and backend emission. Runtime intrinsics are
visible only while checking compiler-provided standard modules; application
source cannot invoke them directly. This boundary avoids teaching the parser or
general overload resolver about host services and provides one replacement
point for the typed Toro IR and a future runtime ABI.

### Public C FFI boundary

Public interoperability is represented by ordinary function declarations with
explicit external metadata: the C symbol name, `"C"` calling convention, and
ABI-only parameter/return types. Semantic analysis registers these declarations
normally; type checking validates the ABI surface and records the selected
declaration on calls. The C backend emits deterministic `extern` prototypes and
calls the unmangled C symbol, but emits no function body.

Foreign-call lowering is isolated from ordinary call lowering. Every argument
is evaluated once into an ABI-typed temporary in Toro source order before the C
call, including when named arguments reorder parameter positions. Managed ARC
values, structs, collections, enums, Results, interfaces, and ordinary Toro
strings cannot cross implicitly. `string as c_string` creates the one supported
borrowed immutable view.

`ProjectConfigLoader` accepts only structured `[ffi].libraries` names.
`NativeCompiler` translates them directly to `-l<name>` process arguments; it
does not construct a shell command or accept arbitrary flags. Compiler-private
standard-library intrinsics remain a separate registry and are not rewritten in
terms of the public FFI. This separation gives the typed Toro IR a clear foreign
call/effect boundary.

### Mixed native CMake integration

`cmake/ToroIntegration.cmake` provides the initial mixed-native orchestration
layer. `toro_add_target(NAME ... PROJECT ...)` asks the compiler for validated
project metadata through `toro project-info --cmake`, tracks the manifest and
all `.toro` files beneath its source root, and invokes `toro emit-c -o` through
a custom command. CMake then compiles that generated source as strict C11 into a
normal static-library target. Manifest `[ffi].libraries` become transitive link
dependencies of that target.

The generated source and its static archive are implementation artifacts behind
the CMake target; consuming code links the target rather than naming generated
files. Toro's generated object currently owns `main`. Native C/C++ translation
units can supply C ABI functions that Toro declared with `extern "C"`, while
ordinary Toro functions remain private generated symbols and cannot be called
from native code. This makes the supported direction honest without introducing
exports, callbacks, C++ ABI coupling, or header generation.

CMake performs orchestration only. Manifest validation, module loading,
semantic analysis, type checking, C generation, and FFI rules remain inside the
normal compiler pipeline. First-class CMake language support and finer-grained
per-module artifacts remain deferred.

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
- external C function declarations and ABI type metadata;
- structs/classes/interfaces/enums;
- inheritance/modifiers;
- generics;
- conversion overloads;
- lifecycle declarations;
- Result propagation.
- module imports and top-level export visibility.

### Semantic analysis

Semantic analysis handles scopes, declarations, name resolution, duplicate
declarations, loop/handle bindings, contextual validity such as `self`, and
private-by-default module visibility. Only public declarations from directly
imported modules participate in resolution.

The semantic analyzer and type checker use the current module plus its direct
imports as a module-local lookup environment. They retain declaration
module/export metadata while forming overload sets and nominal types. Nominal
types use stable identities derived from logical module names rather than source
spelling alone. A same-module declaration shadows imports; otherwise colliding
public declarations from multiple direct imports produce an ambiguity error.

`IRLowerer` receives one deterministic dependency-first checked program with
import declarations removed, so each shared module is represented exactly once.
It carries the same logical identities into typed declarations, selected calls,
generic substitutions, dispatch operations, and source origins; absolute
filesystem paths never participate in IR or native naming.

### Typed toro IR boundary

The Phase 8.1 IR is a deterministic, typed representation of modules,
declarations, values, calls, foreign calls, branches, loops, returns,
construction, member/index operations, conversions, collections, enums,
`Result`, exhaustive `handle`, propagation, and virtual/interface dispatch.
Calls retain the exact frontend-selected declaration identity and sorted generic
substitutions, so the backend does not repeat overload or generic resolution.
Nominal declarations retain fields, defaults, enum variants, visibility, weak
field state, constraints, and stable logical identities. Every operation keeps
a module plus one-based source location for diagnostics and inspection.

All checked programs pass through `IRLowerer` before C generation. `IRProgram`
owns the backend contract and contains no AST pointer or compatibility payload.
`CGenerator` consumes declarations, typed operations, selected declaration
identities, substitutions, runtime-value categories, and source origins from IR
only. A backend-private emission model normalizes those semantic operations for
the established C11 emitter; it does not consult frontend nodes or repeat name,
overload, generic, visibility, or type resolution.

The C backend discovers concrete specializations from IR call identities and
substitutions. Runtime-helper discovery likewise walks IR-derived constructions,
calls, aggregates, dispatch operations, collections, and compiler intrinsics.
Both retain dependency-first program order and deterministic insertion order.

The semantic IR initially identifies the inherited lifetime model as
`legacy-arc`. `ArcMaterializer` converts it into `explicit-arc` schedules with
deterministic retain, release, destroy/finalize, managed-assignment,
owned-temporary, and borrowed-temporary events. Each callable keeps an
unoptimized baseline and an active schedule, so lifetime behavior can be tested
before and after optimization. Class lifetime schedules also expose finalization,
strong-field teardown, and non-owning weak slots.

`ArcOptimizer` consumes the existing lifetime facts rather than recomputing
them. Before it runs, `OwnershipTransferAnalyzer` performs deterministic
block-local final-use analysis. It records an explicit transfer when a directly
loaded owning reference is stored into another local, returned, assigned to a
strong class field, or inserted into currently supported managed collection
storage and the source has no later use in that block. Nullable class values,
derived-to-base upcasts, class-backed interfaces, and concrete generic classes
use the same rule. The analysis deliberately does not transfer outer ownership
inside nested control flow, across loops, through weak fields, or across calls.
The ownership summary layer can describe a callee's store/consume behavior, but
a cross-call handoff remains disabled until the IR/backend calling convention
can preserve that ownership edge explicitly.

`BorrowInferenceAnalyzer` follows transfer analysis. It consumes canonical,
parameter-specific ownership summaries and annotates operation-bounded
receiver, argument, and local-alias borrows.
IR reference modes distinguish `own`, `transfer`, `borrow`, and `weak`; borrow
uses carry their operand, source, destination, origin, kind, and proof reason.
The current borrow lifetime is one call/immediate expression or the last proven
straight-line use of a local alias. It is not a general lifetime-variable
system.
Unrelated effects in a callable do not automatically make every parameter
owning. Direct and concrete-generic calls may borrow an observed parameter;
virtual and interface calls require every reachable implementation to be safe.
Straight-line aliases borrow only through their last proven observation while
the original owner remains live. Returns, persistent storage, collection
insertion, escaping forwarding, foreign/unknown ownership boundaries, unsafe
dynamic targets, weak access, reassignment, and branch/loop uncertainty retain
the owning baseline.

`ArcOptimizer` replaces each proven destination retain plus source cleanup
release with a transfer, and elides parameter or local-alias ARC pairs selected
by borrow inference, while preserving the final owning lifetime edge. Transfer
and borrow metadata are disjoint. The C backend consumes those explicit IR
decisions; it does not independently infer moves, borrows, or lifetimes.

`toro ir --arc` prints both schedules, marks elided operations, and includes
before/after, ownership-transfer, and inferred-borrow totals. Parameter
contracts are inspected separately with `toro ir --ownership`. `toro ir --arc
--no-opt` keeps the baseline active for
differential runtime testing. This is internal ownership analysis: Toro adds no
source-level `move`, `borrow`, `owned`, reference, or lifetime syntax.

`toro ir` runs loading, semantic analysis, and type checking, then prints the
stable textual IR. This dump is a developer inspection format, not a serialized
ABI or a user-facing language contract.

### Effect metadata and analysis

`EffectAnalyzer` is a backend-independent pass over typed IR. It derives local
effects from operation kinds, value categories, selected call identities, and a
small compiler-known metadata table for private standard-library intrinsics. It
then propagates callee effects through a deterministic monotonic fixed point.
Recursive and mutually recursive call graphs terminate because summaries only
gain members from the finite effect set.

The current effect taxonomy covers allocation, retain/reference ownership,
release or possible destruction, escape, blocking, filesystem I/O, environment
access, wall-clock queries, process/console I/O, synchronization, foreign calls,
native runtime calls, dynamic dispatch, process termination, and unknown
external behavior. Categories are intentionally orthogonal: for example,
filesystem reads are native, blocking filesystem operations that may allocate,
while a wall-clock query is a native time query and is not filesystem I/O.

Ordinary selected calls use the declaration identity and sorted generic
substitutions already recorded by the frontend/IR lowerer. Generic calls receive
separate deterministic concrete summary identities. Virtual and interface calls
carry `dynamic_dispatch` and conservatively join all known body-bearing methods
with the selected callable signature. If no valid body or direct callee summary
can be proven, `unknown_external` records the uncertainty. Public C FFI calls
are always `foreign_call + unknown_external` because Toro has no source-level
FFI effect annotations.

Each summary retains one deterministic cause per effect. A local cause points to
the originating IR operation; a transitive cause points to the call origin and
direct callee identity. This is deliberately smaller than a complete diagnostic
graph but preserves the information needed for future explainability work.
The pass also publishes deterministic direct local effect sites and resolved
call edges for policy consumers that must reason per runtime site; these do not
change the public effect set or `--effects` dump.

`toro ir --effects` appends these summaries to the ordinary deterministic IR
dump. Effect analysis runs before ARC materialization and normal C generation.
It does not itself alter ARC, allocation, ownership, or native behavior.
Ownership-sensitive passes use the dedicated summaries below rather than
treating a callable-wide effect as a parameter contract. Effect analysis does
not validate performance policy. Phase 10.1 carries the explicit
`performance function` marker through the AST and typed IR. Phase 10.2 adds a
dedicated `PerformanceContractAnalyzer`; Phase 10.3 places it after storage
selection and optimized lifetime planning. It consumes published effect
sites/call edges plus ownership, escape, storage, transfer, borrow, and ARC
results rather than changing `EffectAnalyzer`, rebuilding transitive
call/dispatch analysis, or deriving backend-specific facts.

### Interprocedural ownership summaries

`OwnershipSummaryAnalyzer` is the canonical callee-contract pass. For each
managed receiver or parameter it records a deterministic set drawn from
observe, retain, persistent store, return/forward, consume, escape, weak store,
and unknown. Compact causes retain the originating source location, the direct
or transitive callable, and the proof reason. Managed return summaries
distinguish newly owned values, parameter and receiver aliases, stored-field
aliases, and unknown ownership.

The pass derives local facts from typed IR, then propagates call facts with a
monotonic fixed point over a finite behavior lattice. Direct and mutually
recursive call graphs therefore terminate without an iteration limit. Virtual
calls join every reachable override; interface calls join every relevant
implementation, including inherited implementations. Borrow safety requires
every possible target to remain observation-only. Missing targets and public C
FFI calls produce conservative unknown/escape behavior.

Generic summaries use the selected declaration plus deterministic concrete
substitutions as their identity. This keeps, for example, a non-managed
`identity<int>` distinct from `identity<Player>`. The pass does not introduce a
runtime calling convention or source ownership annotations.

`EscapeAnalyzer` uses parameter behavior and return-alias facts to propagate
allocation identities across calls. `BorrowInferenceAnalyzer` uses the same
summaries for call and receiver borrows, retaining only local alias lifetime
proof of its own. ARC optimization uses observation-only summaries to remove
the corresponding parameter entry/exit pair. Existing transfer analysis stays
block-local until explicit cross-call consumption can be represented safely.

`toro ir --ownership` appends stable callable summaries, parameter causes, and
return relationships to the normal IR dump.

### Strong-reference cycle diagnostics

`StrongCycleAnalyzer` runs after interprocedural ownership summaries and before
escape/storage analysis. It builds a deterministic persistent-ownership graph
from concrete class instances and their strong fields. Inherited fields retain
their declaration origins; concrete generic specializations remain distinct;
and supported `Array`, `List`, `Map` value storage plus persistent enum/Result
payload paths contribute contained class-reference edges. Explicit `weak`
fields, interface views, call parameters, temporary aliases, borrows, and other
non-persistent summary facts do not create graph edges.

Deterministic strongly connected components identify self-cycles and longer
cycles. Each ownership component produces at most one canonical diagnostic,
with the participating field declarations, an ordered cycle path, the ARC
lifetime consequence, and an advisory suggestion to consider a non-owning
`weak` back-reference. The analysis is deliberately type/storage based and
labels findings as possible cycles; it does not claim concrete heap-shape proof.
Obviously unconstructible all-required, non-null field rings are suppressed.

Cycle warnings do not reject a valid program, rewrite an edge, change generated
C, or alter ARC/weak behavior. Strong cycles remain legal and may leak. Toro
does not have a tracing or cycle-collecting runtime.

### Escape analysis

`EscapeAnalyzer` runs after effect analysis and before ARC materialization. It
assigns every managed class construction a stable identity derived from its
callable identity, logical module, source location, and deterministic occurrence
index. Each site is classified as `does not escape`, `escapes`, or `unknown`
and keeps compact reason records with source origins.

The pass carries allocation-site sets through local aliases, reassignment,
nullable/reference wrappers, inheritance upcasts, class-backed interfaces,
concrete generic classes, and Result/enum payloads. A return, top-level/global
store, strong-field store, or supported Array/List/Map store is a definite
escape. Weak-field storage records a non-owning runtime constraint but does not
itself make the target escape. Branch facts are joined conservatively; loop
bodies are joined with the pre-loop state so analysis remains deterministic and
terminating.

Direct calls use the exact declaration identity, generic substitutions, and
ownership summaries. Proven observing parameters preserve locality; stored,
returned, escaping, or unknown parameters propagate escape. Virtual and
interface dispatch join all reachable targets and remain conservatively
`unknown` at allocation sites when any target can escape. C FFI and unresolved
ownership boundaries are always conservative.

`toro ir --escape` prints allocation classifications, causes, origins, and
managed/non-escaping/escaping/unknown counts. Escape annotations do not directly
change allocation or ownership; the following storage-decision pass is the only
consumer permitted to select a different representation.

### Storage decisions and stack-backed classes

`StorageDecisionAnalyzer` runs after escape analysis and before ARC
materialization. It records `heap` or `stack` on each managed allocation site
and copies that decision onto the relevant construction and alias operations.
The decision is deterministic and includes a compact fallback reason.

Stack promotion currently requires a `DoesNotEscape` construction with a direct
local declaration as its lifetime owner. Managed reassignment, indirect
construction ownership, a weak-observation constraint, or any escaping/unknown
classification keeps heap storage. Returns, globals, strong escaping fields,
collections, foreign calls, and uncertain dynamic dispatch therefore use the
heap. Branch- and loop-local declarations qualify when their object remains
inside that lexical lifetime. Strong fields do not recursively promote their
referents; each construction is decided independently.

The C11 backend reuses the ordinary class layout. A promoted object and its
control block are automatic variables, and Toro references remain pointers to
that single object. The existing base embedding, vtable/interface thunks, init,
field access, and method paths are unchanged. A header bit distinguishes heap
and stack storage so the common finalizer never frees automatic storage. The
owning lexical cleanup invokes finalization exactly once, including derived to
base `destroy` order and strong/weak field cleanup. ARC operations solely needed
to own local aliases are omitted; calls may still use conservative balanced
retain/release operations.

Weak-observed targets are deliberately not promoted. Although each stack object
uses an automatic control block for existing dispatch/layout compatibility, the
current proof does not extend that block for arbitrary zeroing weak slots.
Preserving weak safety takes precedence over promotion coverage.

Heap-only storage decisions are available programmatically for differential
tests. Stack and heap modes must preserve output, identity, dispatch, destruction
order, and cleanup. The optimization changes no Toro syntax or type semantics.

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

### Standard-library runtime boundary

Generated C helpers implement the current filesystem, environment, time, and
string services only when their associated intrinsic is reachable. Concrete
`Result<string, string>` and `Result<bool, string>` values reuse the existing
tagged-union lowering. Native-produced immutable strings are registered in a
small process-lifetime allocation list and released with `atexit`, preventing
dangling pointers while keeping ownership mechanics out of general string
expressions. This representation is intentionally isolated and may be replaced
when Toro gains a unified owned-string runtime.

Standard-library module identities and helper names are deterministic; no
absolute compiler, standard-library, or project path enters generated symbols.

### Structs

Concrete structs lower to C structs with value semantics.

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

### Generic monomorphization

The backend discovers reachable concrete generic calls and type instances from
type-checker annotations, then monomorphizes them before emitting C. Function,
struct, class, and method specializations retain their source declaration plus a
concrete substitution map. Deterministic keys recursively mangle every concrete
type argument and feed the existing C naming helpers.

After substitution, specializations reuse ordinary lowering rather than a
parallel runtime representation. Generic structs therefore remain values, and
generic classes use the existing ARC/weak/lifecycle, inheritance, vtable, and
interface paths. This is compile-time specialization only: the runtime gains no
type descriptors, erasure layer, reflection, or RTTI.

### Overloads and explicit conversions

Type checking records the exact function, method, or interface declaration
selected for each call. The backend consumes that declaration identity together
with any concrete generic substitutions; it does not repeat overload resolution.
Overload symbols include a deterministic source-signature mangle, while generic
specializations additionally include their concrete type arguments. The same
identity flows through inherited lookup, virtual slots, interface thunks, and
named-argument ordering.

Built-in `int`/`dec` casts lower directly to C conversions. A class or struct
`overload as Type` declaration lowers to a receiver function selected by the
typed cast annotation. Struct operands preserve value semantics, and temporary
class operands use the ordinary ARC call-temporary path so each operand is
evaluated once and released exactly once.

### Core collections

Reachable concrete collection instances receive deterministic generated C
types and a small reusable helper family per specialization. `Array<T>` is a C
value containing a count and owned contiguous element buffer; copying in Toro
invokes a deep buffer clone. `List<T>` is a pointer to a growable,
reference-counted contiguous backing object. `Map<string, V>` is a pointer to a
reference-counted open-addressed hash table. Ordinary List/Map assignment retains
the backing object, while `.clone()` allocates independent storage.

Element helpers isolate retain/release behavior for class references and nested
shared collections. Array/List access uses generated checked get/set helpers,
and Array/List `for ... in` lowers to indexed traversal. Strings remain immutable
`const char*` values; this is sufficient for the current literal-oriented string
semantics and string-keyed maps, so no owned string runtime is introduced.

### Backend hardening baseline

Backend discovery and emission use explicit insertion-order registries for
concrete types, functions, methods, Results, collections, vtables, and interface
tables. Lookup maps do not determine generated declaration order. Native names
derive only from source declarations, callable signatures, and recursively
mangled concrete types. Repeated compilation of the same program is therefore
covered as a byte-identical generated-C invariant.

Concrete Result and collection registration validates runtime boundaries before
helper emission. Generic specialization registries diagnose identity collisions
rather than silently selecting an unrelated declaration. Shared backend helpers
prepare call arguments, preserve Array value-copy semantics, retain managed
parameters, and release owned call temporaries in reverse order. These helpers
remain localized behind the C backend while the typed IR compatibility boundary
is migrated incrementally; they are not duplicated in `IRLowerer`.

Representative generated output is syntax-checked as strict C11, and a larger
native integration test exercises inheritance, virtual and interface dispatch,
generics, overloads, explicit conversions, collections, Result, enums, ARC, and
lifecycle cleanup together.

## Current known runtime boundaries

Important unsupported or deferred runtime features include:

- generic interfaces and generic interface methods;
- interface-valued fields;
- class-reference struct fields;
- RTTI/dynamic casts;
- multiple inheritance;
- base-initializer chaining;
- Map iteration and non-string Map keys;
- nested Array elements and interface-valued collection elements;
- collection-valued struct/class fields;
- multiple configured source roots, workspaces, and project-wide build planning
  beyond one `toro.toml` application;
- import aliases, wildcard imports, and package dependencies;
- source-level module qualification for disambiguating colliding public imports;
- separate object-file compilation and linking per module;
- raw pointers, C aggregates, callbacks, variadics, header parsing, dynamic
  loading, and C++/Objective-C/Rust ABI interoperability;
- interprocedural ownership/escape summaries and broader borrow optimization
  beyond the current operation-bounded parameter, receiver, argument, and
  straight-line local-alias facts, direct-local stack promotion,
  allocation-site escape annotations, and block-local final-use transfer;
- general memory/performance observability and policy categories beyond the
  current contract-specific final-plan report.

The type system may understand some concepts before the C backend can execute them. The backend should reject unsupported constructs clearly rather than emit incorrect C.

## Near-term architecture principle

The completed Phase 6 C backend is a semantic proving ground and the runtime
baseline for Phase 7 application work.

Do not add ARC or allocation heuristics inside the C generator. Lifetime
materialization and optimization belong in explicit IR passes before backend
emission.

Correct runtime behavior is the priority.

## Typed toro IR and future analyses

Toro now has a typed intermediate representation, deterministic effect and
interprocedural ownership summaries, allocation-site escape annotations,
explicit storage decisions, block-local ownership transfers, operation-bounded
borrow facts, and baseline/optimized ARC schedules between static semantics and
an IR-only C backend. Broader stack allocation, longer-lived borrow reasoning,
cross-call consume handoff, and performance work remain layered on that
foundation.

Target pipeline:

```text
source
    ↓
frontend
    ↓
typed toro IR
    ↓
effect analysis
    ↓
ownership-summary analysis
    ↓
strong-cycle diagnostics
    ↓
escape analysis
    ↓
storage decisions
    ↓
ARC materialization
    ↓
ownership-transfer analysis
    ↓
borrow inference
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

If the callee cannot retain `user`, the compiler avoids the parameter
retain/release pair and records an operation-bounded borrow. Longer-lived or
ambiguous borrowing remains conservative.

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

If `enemy` does not escape or participate in an incompatible weak/lifetime
relationship, the storage-decision pass may stack allocate it while retaining
class/reference semantics at the source level.

## performance function architecture

`performance function` is a compiler-enforced execution contract. Phase 10.1
activates its canonical syntax and preserves a dedicated contract marker
through typed IR and the detached backend model. Phase 10.2 evaluates marked
callables against existing transitive effect summaries, producing distinct
satisfied, violated, and unproven results. Phase 10.3 evaluates those rules
after the canonical runtime plan is complete. The C backend intentionally does
not alter generated code merely because the marker is present.

It is not merely an optimization level.

The current effect-policy pass rejects:

- general heap allocation;
- blocking I/O;
- sleeping/waiting;
- hidden synchronization;
- escaping temporaries;
- allocating conversions;
- calls whose effects violate the contract.

Unknown external or foreign behavior is reported as unproven rather than as a
known prohibited operation. Each managed construction is matched to its escape
and storage decision. A stack-backed site discharges only its own allocation
effect; heap-backed sites and non-class runtime allocations remain prohibited.
Transitive helper, recursive, virtual/interface, and concrete generic behavior
is refined through direct edges already published by effect analysis.
Ownership transfer, inferred borrowing, and the optimized ARC schedule are
final inputs rather than new policy rules—retain/release are not independently
forbidden today.

Phase 10.4 adds a separate `PerformanceContractReporter`. The analyzer remains
the sole verdict authority. The reporter consumes structured results, local
effect sites, resolved direct/dynamic call edges, escape/storage reasons, and
final ARC schedules to produce concise diagnostics and deterministic
`toro ir --performance` output. It finds canonical shortest simple paths, so
recursive graphs terminate without expanding cycles indefinitely. Satisfied
reports expose only material discharge/ownership/dispatch proofs; failed and
unproven reports retain terminal source origins and concrete generic or dynamic
target identities. General Phase 11 observability remains separate.

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
- deeper instance/path cycle proofs and optional ownership-graph inspection;
- broader consumers of the current interprocedural ownership summaries;
- LLVM or another backend after the IR is mature;
- CMake integration for mixed native projects;
- first-class CMake language support only much later if justified.

These are roadmap items, not current implementation requirements.
