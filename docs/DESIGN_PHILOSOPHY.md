# Toro Design Philosophy

## Thesis

Toro is a **performance-scalable native application language**.

The language should let programmers write ordinary native application code without requiring the entire program to feel like systems programming. The compiler should progressively prove away unnecessary lifetime and runtime costs, while developers can request stronger guarantees in genuinely performance-critical code.

The guiding principle is:

> **Toro should have ownership analysis, not ownership programming.**

A complementary product principle is:

> **Automatic performance behavior should not be opaque performance behavior.**

Normal Toro should remain ergonomic. The compiler should optimize ownership and lifetime behavior underneath that source model. `performance function` should provide enforceable execution guarantees where required. Inspection tooling should explain what the compiler actually decided.

## The four-layer model

Toro's long-term programming model has four reinforcing layers.

### 1. Normal Toro code

Ordinary application code should optimize for readability and correctness.

Programmers should normally think in terms of:

- values;
- objects;
- relationships;
- functions;
- modules;
- errors;
- domain behavior.

They should not be forced to annotate routine code with moves, borrows, retain counts, lifetimes, or allocation strategies.

### 2. Compiler intelligence

The compiler should increasingly understand:

- ownership transfer;
- escape behavior;
- temporary borrowing;
- stack eligibility;
- heap requirements;
- reference sharing;
- ARC retain/release necessity.

The source-level semantics remain stable while the compiler is free to choose a cheaper legal implementation.

A class remains a reference type with identity even when a future optimizer proves that a specific instance can safely live on the stack.

### 3. Performance contracts

Normal functions ask the compiler to produce correct native code and optimize it where possible.

`performance function` asks the compiler to **prove that the transitive call
tree obeys a predictable execution contract**. The canonical syntax and typed
IR marker are active, and transitive effect-policy validation now rejects known
prohibited or unprovable behavior. Contract satisfaction is evaluated against
the proven final runtime plan: stack-promoted construction is not treated as a
heap allocation, while fallback heap storage and real escapes remain visible.
Contract verdicts are now explainable through `toro ir --performance` and
concise multi-hop diagnostics. This explanation layer consumes the analyzer's
structured verdict and proven runtime plan; it does not reinterpret policy.
Broader memory and performance observability remains staged in Phase 11.

Potential prohibited behavior includes:

- general heap allocation;
- blocking filesystem/network I/O;
- sleeping/waiting;
- hidden locks or synchronization;
- escaping temporary allocations;
- hidden allocating conversions;
- calls into functions that violate the same contract.

This is not a promise that a function is algorithmically fast. It is a guarantee about classes of runtime behavior.

### 4. Observability

Compiler automation should be inspectable.

The first general surface is `toro inspect`, which summarizes already-proven
runtime decisions—allocation/storage, dynamic dispatch, optimized ARC,
borrowing/transfers, and blocking/synchronization effects—without changing
those decisions. `toro inspect --memory` provides the detailed static memory
report: allocation sites, final storage, ARC before/after optimization,
borrows, transfers, weak fields, and possible strong cycles. It is intentionally
not a runtime profiler. `toro explain-memory file.toro:line[:column]` now turns
those finalized compiler facts into a targeted explanation. Ambiguous lines are
reported rather than guessed, and the explainer cannot change analysis or
optimization decisions.

Future tooling should be able to answer questions such as:

- Why was this object heap allocated?
- Why was this reference retained?
- Why could this object not be stack allocated?
- Did this call borrow or own its argument?
- Which retains/releases were removed?
- Why does this `performance function` violate its contract?
- Where are potential strong-reference cycles?

The desired experience is:

> **You should not have to manage it manually, but you should be able to understand it precisely.**

Toro now applies that principle to persistent strong-reference topology: the
compiler warns about deterministic possible ARC cycles, identifies the owning
field path, and points to `weak` as the explicit non-owning escape hatch. These
diagnostics are advisory. The compiler neither silently weakens references nor
pretends that ARC includes cycle collection.

## Value and reference semantics

Toro keeps a strong conceptual distinction.

### Values

`struct` and primitive/value types are true values.

They should be eligible for:

- registers;
- stack storage;
- copies;
- moves;
- scalar replacement;
- arena storage where explicitly appropriate later.

They should not become ARC-managed merely because the runtime supports ARC.

Phase 12.1's arena investigation reinforces the evidence-first boundary. Small
current-language frame, parser, and request workloads reveal plausible
common-lifetime heap graphs, but static compiler facts alone do not show that
allocator or ARC cost is material. Arena design therefore remains deferred
until a representative application and profiling demonstrate a residual cost
that existing analysis, scalar replacement, collection improvements, or
reusable storage cannot address. Toro should not acquire lifetime syntax merely
because a region allocator is technically possible.

Application Milestone A1 adds the first real simulation evidence. Its
collection-backed tick-local effects remain heap allocated even though their
logical lifetime ends every tick, while ownership transfer already removes
part of the ARC traffic. That is a useful candidate pattern, not runtime cost
proof. The same application exposes independently updatable actor/effect work,
but without scaling or contention evidence it also leaves concurrency design
deferred. This is the intended evidence-first progression: build applications,
observe real constraints, profile separately, and only then authorize new
memory or execution semantics.

### References

`class` values have identity and reference semantics.

The source model is simple:

- class references are strong by default;
- `weak` is explicit;
- strong references keep an object alive;
- weak references observe without owning and become `null` when their target dies.

The compiler may eventually optimize the implementation of those semantics.

The programmer should not need to know whether a particular reference happened to require ARC internally unless they ask the tooling.

## ARC is not Toro's identity

Toro currently uses ARC to implement class lifetimes correctly. That is the correct baseline.

Long term, ARC should be used when shared lifetime actually requires it.

The optimization hierarchy is conceptually:

```text
value semantics
    ↓
stack/register storage
    ↓
ownership transfer / move analysis
    ↓
escape analysis
    ↓
borrow inference
    ↓
ARC where shared lifetime remains necessary
    ↓
ARC retain/release elision
```

This is being implemented gradually now that correct runtime semantics and a
suitable toro IR exist. Toro now materializes the current ARC schedule,
transfers ownership through straightforward block-local final uses, and removes
ARC pairs selected by operation-bounded borrow inference. The
compiler also classifies managed allocation sites as local, escaping, or unknown
while propagating aliases and recording escape causes. A separate storage pass
now promotes direct local owners to automatic storage when identity, dispatch,
and lifecycle behavior remain provably bounded, while heap + ARC remains the
conservative fallback. Weak-observed and uncertain lifetimes are intentionally
not promoted. A canonical interprocedural ownership-summary pass now describes
each managed parameter independently, records managed-return relationships,
reaches a deterministic fixed point through recursion, and conservatively joins
dynamic dispatch targets. Escape analysis and the following borrow pass consume
those facts, so wrapper chains can preserve locality and operation-bounded
non-owning calls can remove ARC traffic without changing the source model.
Broader stack placement, longer-lived borrow reasoning, cross-call ownership
handoff, and the rest of the optimization hierarchy remain future work.

## Why not expose ownership everywhere?

Toro is not trying to reproduce Rust's source-level ownership model or C++'s manual lifetime complexity with different spelling.

Explicit ownership syntax may eventually exist for advanced or ambiguous situations, but it should not dominate normal application code.

The preferred default is:

```toro
player := Player(name: "Austin")
world.add(player)
```

The compiler should determine whether that implies:

- a borrow;
- a move/ownership transfer;
- a retain;
- an escape to the heap;
- or another legal storage strategy.

Only when static analysis cannot safely determine the intended semantics should additional programmer guidance be considered.

## Target domain

Toro is primarily aimed at **native application development**, especially software where most code should be comfortable but some code requires predictable execution.

Likely strong domains include:

- games and simulations;
- audio/video/media processing;
- native desktop applications;
- networking software;
- high-throughput or latency-sensitive services;
- CLI tools;
- compilers, parsers, and developer tools;
- data-processing applications.

Toro is not initially optimized for kernels, drivers, bare-metal firmware, or allocator implementation. C, C++, and Rust are naturally stronger fits when explicit low-level memory control is the entire job.

## Performance scalability

Toro's desired developer experience is:

1. Start with ordinary readable code.
2. Let the compiler remove unnecessary lifetime and allocation costs.
3. Inspect what the compiler chose when performance matters.
4. Apply `performance function` only to hot code that needs hard guarantees.
5. Use lower-level/advanced mechanisms only when measurement proves they are necessary.

This is the core idea behind **performance-scalable native applications**.

## Correctness before optimization

The compiler should not prematurely complicate its architecture for hypothetical optimizations.

The development order is:

```text
correct semantics
    ↓
real applications
    ↓
measurement and observability
    ↓
compiler ownership optimization
    ↓
performance contracts and advanced allocation strategies
```

A straightforward but correct ARC implementation is acceptable before ownership analysis exists.

## Long-term differentiation

Toro is not justified merely by cleaner syntax, ARC, or tagged enums.

Its strongest potential differentiator is the combination of:

- native execution;
- ergonomic ordinary code;
- compiler-inferred ownership/lifetime optimization;
- explicit enforceable performance contracts;
- first-class explanation of allocation, ownership, and runtime decisions.

The long-term question Toro must answer is:

> **Can native software become meaningfully easier to write without giving up the ability to demand and understand low-level performance?**

The roadmap should be evaluated against that question.
