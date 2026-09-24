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

A future `performance function` additionally asks the compiler to **prove that the transitive call tree obeys a predictable execution contract**.

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

This should be implemented gradually after correct runtime semantics and a suitable toro IR exists.

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
