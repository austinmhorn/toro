# Phase 12.1B — arena design and alternatives specification

## 1. Executive summary

Application Milestones A1 and A2 justify reopening arena work at the design
level. They do not yet justify implementation. The representative simulation
has one dominant bounded-lifetime pattern: 48,000 `FrameEffect` constructions
per baseline run produce 96,000 object/control-block allocations. Allocation
and final lifecycle account for a material share of an allocation-amplified
native sample, while memory remains bounded and leak-free.

This specification compares arenas with collection reservation/reuse, pooling,
and stronger stack/lifetime analysis. Collection improvements are a serious
alternative: reservation plus tick-list reuse could theoretically remove
26,000 of 122,268 baseline allocator invocations. They cannot remove the
96,000 per-object/control allocations or the corresponding final lifecycle.
That residual distinction must be measured rather than assumed.

If a region mechanism is eventually justified, the preferred conceptual model
is a **compiler-inferred, lexically anchored, single-threaded region with
conservative heap fallback**. It preserves ordinary Toro source and class
semantics. Region-to-region references use region lifetime rather than
per-edge ARC; region objects retain ordinary heap targets; heap-to-region
references are prohibited unless a future proof establishes that the heap
owner cannot outlive the region. Exactly-once lifecycle cleanup occurs before
bulk storage release. Weak targets, FFI escape, and uncertain cross-region
references are excluded from an initial model.

The recommendation is:

```text
Prototype alternative first
```

The next separately authorized experiment should add only the collection
capacity/clear/reuse capability needed to rerun A2. If `FrameEffect`
allocation/finalization remains a material top hotspot after collection growth
and list-object churn are removed, a later narrow internal arena prototype is
justified. This milestone adds no syntax, parser/AST changes, IR operations,
runtime allocation, backend lowering, or performance-policy changes.

## 2. A1/A2 evidence

A1 established the static shape:

- `FrameEffect` has class identity and is inserted into
  `List<FrameEffect>`;
- the list and its elements die at the tick boundary;
- collection storage makes the construction escape its lexical expression;
- storage selection therefore keeps it on the heap;
- ownership transfer already removes the insertion retain and local cleanup
  release;
- ordinary stack promotion, borrowing, and ARC optimization do not remove the
  final heap allocation/lifecycle.

A2 established runtime materiality:

| Baseline fact | Count/share |
| --- | ---: |
| `FrameEffect` constructions | 48,000 |
| object/control `malloc`/`calloc` calls | 96,000 |
| all generated `malloc`/`calloc` calls | 100,262 |
| collection and other `realloc` calls | 22,006 |
| all generated allocator invocations | 122,268 |
| effect object/control invocations | 78.5% |
| effect allocation CPU samples | 13.6% |
| effect release/finalization/free CPU samples | 23.3% |
| event-list add/growth CPU samples | 10.7% |
| effect-list add/growth CPU samples | 1.3% |

Peak memory stayed approximately flat as tick count increased, and the leak
check reported zero leaks. The problem is repeated allocator and lifecycle
work, not unbounded memory growth. The CPU percentages come from one
allocation-amplified sample and are not exact baseline speedup predictions.

## 3. Problem statement

Toro can prove that each effect graph is bounded by one tick, but its current
storage lattice selects only stack or heap for each managed allocation site.
Once a dynamically sized owning collection receives a class reference, the
object outlives its construction expression and is conservatively heap-backed.
Per-object identity and collection ownership then require an object allocation,
a weak-control allocation under the current universal class layout, final
release, finalization, and two frees.

The design question is not whether a bump allocator is possible. It is whether
Toro can select common-lifetime storage without exposing ownership programming,
changing class semantics, weakening lifecycle guarantees, or creating dangling
references. The solution must also beat simpler changes that remove collection
churn.

## 4. Alternatives comparison

### Collection reservation and reuse

The baseline performs 22,006 growth `realloc` calls. Of those, 22,000 are the
per-tick event/effect list growth pattern; the remaining six grow the persistent
actor list. It also allocates 4,000 per-tick list objects.

A capacity-aware experiment could:

- reserve `actors + 2` event slots and `effects_per_tick` effect slots;
- retain the two list backing objects across ticks;
- clear logical elements between ticks;
- release effect elements during `clear`, preserving current lifecycle;
- reuse backing storage without repeated growth.

In the ideal accounting this removes 22,000 growth calls and 4,000 list-object
allocations: 26,000 of 122,268 allocator invocations, or 21.3%. It may also
remove much of the sampled list-growth cost. It does **not** remove:

- 96,000 `FrameEffect` object/control allocations;
- effect initialization and identity;
- final release/finalization/free for every effect;
- the need to clear owned elements correctly;
- any object-level ARC/lifecycle work.

Improved ownership transfer can avoid extra element retain/release pairs, as it
already does, but cannot eliminate the final owner cleanup. Collection-backed
object reuse crosses into pooling and has separate identity/lifecycle risks.

This alternative is smaller, independently useful, and quantitatively
measurable. It must be tested before arena implementation, but the known upper
bound shows it cannot by itself remove the dominant allocator-call class.

### Object pooling

A pool sized for the maximum live effects could reuse object and control-block
storage after the first tick, potentially removing most later effect allocator
calls. It also retains per-object initialization/reset and some form of
liveness bookkeeping.

Application-managed pooling is a poor default for Toro:

- callers must acquire, reset, and return objects manually;
- stale aliases can observe a logically reused identity;
- weak references need a new generation or control-block policy;
- `destroy()` meaning becomes ambiguous between pool return and final pool
  destruction;
- every field, including future resource-owning fields, needs a correct reset;
- early returns and errors make return-to-pool discipline visible in source;
- capacity and overflow policy become application ownership plumbing.

A compiler/runtime pool could hide some mechanics but still needs region-like
lifetime proof to prevent stale aliases. Once all objects are known to die
together, bulk region storage is simpler than maintaining a free list and
recycling identities individually. Pooling remains useful for domain-specific
long-lived entities, but it does not fit the general “ownership analysis, not
ownership programming” solution for this batch.

### Improved stack and lifetime analysis

Stronger analysis could prove the whole list/object graph bounded by the tick,
but ordinary stack allocation is not a general representation for it:

- element count is runtime-sized;
- class addresses must remain stable while stored in the list;
- recursive/nested graphs can make required stack size data-dependent;
- large actor/effect inputs could overflow a thread stack;
- list backing storage is itself dynamic;
- aliases and virtual/interface views must retain identity;
- cleanup must still run on every control-flow exit.

Scalar replacement can eliminate an object only when its identity and aliases
are unobservable. A `List<FrameEffect>` with runtime iteration is not generally
scalar-replaceable. A compiler-managed dynamic stack segment would effectively
be a region allocator under another name. Stronger lifetime analysis is still
required to prove region eligibility, but ordinary C stack promotion is not a
realistic general solution for variable-sized batches.

### Compiler-inferred regions

Inference best matches Toro's philosophy:

- source remains ordinary classes and collections;
- the compiler can conservatively keep uncertain sites on the heap;
- existing escape, ownership-summary, transfer, borrow, and storage facts form
  a foundation;
- tooling can explain the selected common lifetime.

Its risks are substantial:

- a small source edit may change region eligibility and performance;
- cross-call lifetime inference needs deterministic summary semantics;
- heap fallback can make performance unpredictable unless explained;
- dynamic dispatch must be safe for every target;
- diagnostics must distinguish safety fallback from missed optimization;
- `performance function` cannot rely on an inferred growable region whose
  capacity is not proven.

Inference is the preferred eventual source model, but it needs an explicit IR
region decision and first-class observability. It must not be reconstructed in
the C backend.

### Explicit or scoped arena

A lexical scoped arena is more predictable: entry establishes a lifetime,
allocations are visibly grouped, and exit defines teardown. An explicit arena
object also makes capacity and destination selection visible.

Both approaches introduce ownership/lifetime programming into ordinary code.
They force APIs to accept arena context or require new scope syntax, and they
make otherwise identical class construction depend on programmer-selected
storage. No evidence yet shows that inference fails or that users need manual
placement. This specification therefore defines no spelling. A future lexical
opt-in should be considered only if an inferred prototype cannot provide stable
decisions or required capacity guarantees.

## 5. Proposed conceptual model

If the alternative experiment leaves the measured object lifecycle material,
the preferred arena model is:

1. The compiler identifies a lexical or callable batch boundary already
   present in control flow.
2. It creates an internal `RegionIdentity` for allocations proven not to
   escape that boundary.
3. Eligible class objects keep normal identity, layout, dispatch, and mutation,
   but their storage comes from the region.
4. References wholly inside the same region are governed by region lifetime,
   not per-edge ARC.
5. Strong edges from region objects to ordinary heap objects retain normally
   and release during region teardown.
6. Unsafe outbound references force conservative heap fallback for the
   affected graph; no dangling reference is tolerated.
7. Teardown performs required lifecycle/resource cleanup exactly once, then
   releases all region storage in bulk.

The initial model is single-threaded, inferred, and conservative. “Arena” is a
storage decision, not a source-level class kind or ownership type.

## 6. Lifetime rules

The conceptual lifetime boundary is **lexically anchored** and may coincide
with a block, loop iteration body, or callable activation. A callable-scoped
region is a special case of a lexical region. An explicitly managed open-ended
arena is not part of the initial model because it obscures the end point the
compiler must prove.

Regions may be nested internally. Each region has one begin point, one logical
end, and cleanup on every exit edge, including early return and Result
propagation. Loops create a fresh logical region per iteration unless analysis
selects reusable storage whose contents are torn down/reset at each iteration.

Eligibility requires proof that every region-backed reference is dead by the
region end. Uncertain calls, returns, global/module storage, FFI, unrelated
region storage, or a longer-lived owner cause heap fallback. The compiler must
not reject otherwise valid ordinary Toro merely because an optimization proof
fails.

## 7. Reference-direction rules

| Relationship | Conceptual rule |
| --- | --- |
| arena object → same arena object | Allowed; region lifetime dominates and per-edge ARC is unnecessary. |
| arena object → ordinary heap object | Allowed; retain heap target and release it during teardown. |
| ordinary heap object → arena object | Prohibited in the initial model; use heap fallback unless a future owner-lifetime proof is explicit in IR. |
| stack/local → arena object | Allowed only while the local is proven within the region lifetime. |
| inner arena → outer arena | Allowed because the outer region outlives the inner, subject to outer-object semantics. |
| outer arena → inner arena | Prohibited; the outer reference could survive inner teardown. |
| unrelated arena → arena object | Prohibited without a proven nesting/lifetime ordering. |

The dangerous case is not determined by physical storage alone. A heap-backed
collection may itself be proven region-bounded, but the initial prototype
should conservatively treat storing an arena reference into heap-managed
storage as an escape unless that collection has the same explicit IR region
provenance. There is no source cast or annotation that bypasses this rule.

## 8. ARC interaction

Three models were considered:

- **Full ARC inside the arena:** easiest semantic reuse, but retains most
  retain/release and final-release traversal measured by A2.
- **No ARC anywhere:** fast but incorrect for heap targets and resource-owning
  fields.
- **Hybrid region ownership:** region lifetime replaces ARC only for references
  wholly within one region; ordinary heap edges keep normal ARC.

The hybrid model is preferred. A region allocation begins alive and remains so
until teardown. Same-region strong aliases do not increment a count. Region
objects that own heap objects perform ordinary retains, recorded as teardown
obligations. Heap objects cannot own region objects in the initial model.
Heap-only graphs retain current ARC unchanged.

This requires reference region provenance in IR. ARC materialization must
consume the storage decision and omit only operations proven redundant under
that region lifetime. Ownership transfer and borrow inference remain separate
analyses. The backend must not infer region ownership or suppress ARC itself.

## 9. Destruction semantics

Toro's exactly-once lifecycle guarantee is preserved.

Region teardown conceptually performs:

1. close the region to new allocations;
2. invalidate any supported weak targets before their storage can disappear;
3. visit nontrivial objects in deterministic reverse allocation order;
4. run each object's `destroy()` chain exactly once, derived to base;
5. release that object's ordinary heap-owned strong fields and clear weak
   observation slots after `destroy()` returns;
6. release collection/runtime resources registered with the region;
7. free or reset region storage in bulk.

All object bytes remain allocated until lifecycle traversal finishes, so
`self` remains addressable during `destroy()`. Toro currently guarantees that
the object's own fields are usable during destruction; it does not guarantee
that an arbitrary peer's `destroy()` has not already run. Reverse allocation
order is deterministic, cheap to record, and works in the presence of cycles.
Ownership-topological order is rejected because cyclic graphs have no such
order and graph traversal would add cost and semantic complexity.

Objects with no `destroy()`, no heap-owning fields, no weak slots, and no other
runtime cleanup are trivially destructible. They need no destructor-list entry.
This is important for the measured `FrameEffect` shape: a future region could
avoid both per-object free and destructor registration, but A2 does not measure
the speedup of that representation.

### Strong cycles

A same-region strong cycle does not leak storage after bulk teardown. It can
still affect observable lifecycle order, and any edge leaving the region keeps
ordinary ownership consequences. Strong-cycle diagnostics therefore remain
relevant. A future reporter may distinguish a fully contained region cycle
from a heap/mixed cycle, but the compiler must not globally suppress warnings
merely because one allocation is region-eligible.

## 10. Weak-reference semantics

Zeroing guarantees cannot be weakened. Options are:

1. prohibit region objects as weak targets;
2. allocate weak control blocks separately on the heap and invalidate them at
   teardown;
3. keep control blocks in the region only when every weak slot is also proven
   to die no later than the region;
4. introduce a different generation-based weak scheme.

The initial conceptual/prototype boundary chooses option 1. Any object that may
be weakly observed falls back to ordinary heap storage. This matches current
stack-promotion conservatism and lets the dominant non-weak `FrameEffect` path
avoid its universal heap control block in a future prototype.

Region objects may hold weak references to ordinary heap targets using the
existing control-block mechanism; teardown must clear those slots. Supporting
region weak targets later requires option 2 or a fully proven region-local weak
graph. A separately heap-backed control block preserves external weak slots but
retains one of A2's two per-object allocations, so its cost must be measured.

## 11. Collection semantics

Collection object, backing storage, and contained values are separate storage
decisions.

### `Array<T>`

An Array remains a value with independent-copy semantics. Its backing storage
may use a region only when the Array and every copy are proven region-bounded.
Copying must still produce logically independent storage. An escaping copy
uses ordinary heap storage or forces fallback.

### `List<T>`

A List remains reference-backed and shared under ordinary assignment. Region
backing is safe only when every alias is bounded by the same region. Growth may
allocate a new region block and leave the old block to bulk teardown; this is
fast but can retain more peak memory than `realloc`. A reusable-region runtime
could preserve chunks across ticks.

For `List<ArenaClass>`, same-region element edges need no ARC. A normal heap
List cannot contain an arena reference unless the compiler records and proves
that the List itself has matching region provenance. Otherwise element
insertion forces heap fallback.

### `Map<string,V>`

Map tables follow the same alias/provenance rule as List. Region growth leaves
old tables until teardown unless the allocator supports reusable blocks.
Keys and values retain their existing value/reference semantics. A heap target
stored as a value is retained normally and released at teardown.

### Clearing and cloning

`clear` in the collection alternative must release ordinary owned elements and
reset count without shrinking capacity. Under a future same-region collection,
clearing may logically destroy elements early only if lifecycle semantics are
preserved; otherwise the region retains them until teardown.

`.clone()` remains an independent collection. A clone inside the same region
may use that region. A clone that escapes cannot carry arena references; it
requires a semantics-preserving heap representation, which for identity-bearing
objects generally means the original graph must have used heap storage. The
compiler should fallback rather than silently deep-copy class identities.

No rule says “all collections in a region use arena storage.” Each collection
and contained reference needs one compatible provenance decision.

## 12. Generic, interface, enum, and Result semantics

Region eligibility is computed after concrete generic substitution. `Box<T>`
does not have one universal storage behavior: `Box<int>` and
`Box<SomeClass>` may create different ownership edges, while preserving their
existing concrete type identities. No generic arena syntax is needed.

A class-backed interface value is a view of the same object and carries its
region provenance. It may be used only within a compatible lifetime; converting
to an interface does not force a new object or permit escape. Struct-backed
interface values remain value copies, but embedded managed references retain
their own provenance restrictions.

Enums and `Result<T,E>` remain values. A payload may contain an arena reference
only while the containing value stays within the same or shorter lifetime.
Returning, storing, or propagating such a payload outside the region forces
heap fallback for the referenced object. Exhaustive `handle` does not change
the lifetime. Postfix `?` is an exit edge and therefore participates in region
cleanup before propagating an error.

Inheritance, virtual dispatch, and concrete generic class identity remain
unchanged. Every possible dynamic target must accept the same proven lifetime;
uncertain dispatch falls back to heap storage.

## 13. FFI boundary

Arena-backed class references do not cross C FFI in the initial model. Current
FFI types already exclude Toro class references, which is the safe baseline.
A borrowed primitive/string view may continue to cross only under its existing
rules; arena storage must not silently extend that ABI.

If future FFI accepts managed references, an unknown C call is assumed to
retain or escape them. Without a statically enforceable no-escape ABI contract,
the allocation falls back to the heap. This milestone introduces no FFI
annotation for regions or lifetimes.

## 14. Performance-contract interaction

No current performance policy changes.

A future region allocation may count as allowed bounded allocation only when
all of the following are proven in the final runtime plan:

- region storage is initialized before the contracted path;
- capacity is fixed or otherwise bounded for that path;
- allocation cannot call the general heap allocator;
- exhaustion behavior is deterministic and policy-approved;
- no hidden synchronization exists;
- teardown work, including destructor-list length, is bounded;
- no fallback to general heap is possible.

A chunked or growable inferred region remains a general allocation effect when
growth can call the heap. A reusable fixed-capacity region may eventually
satisfy the contract, but only after policy explicitly recognizes the new
storage category. `EffectAnalyzer` should continue to describe allocation;
`PerformanceContractAnalyzer` should consume the finalized region/storage
facts, following the existing “analysis decides; reporting explains” boundary.

## 15. Failure and capacity model

Four policies have different consequences:

| Policy | Semantics | Performance-contract consequence |
| --- | --- | --- |
| fixed capacity, fail | predictable bound; needs a language/runtime failure result | potentially acceptable once specified |
| grow chunks | preserves ordinary execution until system allocation fails | general heap effect on growth |
| abort on exhaustion | simple and similar to current allocation failure | predictable but harsh; not silently acceptable |
| fallback to heap | preserves progress but creates mixed provenance | unpredictable and unsafe unless ownership is reclassified before execution |

The preferred normal-code concept is a growable chunked region whose allocation
failure follows the current process-failure baseline. It is an optimization,
so compile-time proof failure falls back to ordinary heap storage; **runtime
exhaustion must not switch individual objects to heap**, because that would
change provenance and cleanup rules mid-execution.

This normal growable model is not automatically legal in a `performance
function`. A future performance-capable mode needs preinitialized bounded
capacity and an explicitly specified exhaustion contract. Phase 12.1B does not
choose that language-level failure behavior.

## 16. Nested-region analysis

Nested regions are useful for loop iterations, parser subunits, and request
suboperations, but only with a strict lifetime ordering:

- inner references may point to outer or heap objects;
- outer or heap objects may not retain inner references;
- same-inner-region edges use region ownership;
- siblings may not retain one another after either sibling ends;
- inner teardown completes before outer teardown;
- returning an inner object forces heap fallback or promotion to a proven outer
  representation before allocation, never relocation after identity exists.

An initial prototype does not need arbitrary nested region inference. One
lexically anchored region per callable/loop iteration is sufficient to test the
measured workload. The IR design should nevertheless make nesting explicit so
later work does not need a representation redesign.

## 17. Concurrency boundary

Regions are single-threaded in this design. Sharing a region, an arena-backed
object, or its allocator state across threads is unsupported. Phase 12.2 remains
deferred.

Future concurrency would need answers for region ownership transfer, task join
lifetime, cross-thread references, allocator synchronization, atomic weak
controls, and teardown coordination. None can be inferred from A2, which found
no measured parallel benefit. Region reopening does not authorize atomic ARC
or local/shared ARC specialization.

## 18. IR design sketch

A future implementation would minimally add backend-independent facts
equivalent to:

- `RegionIdentity`: stable callable/source/occurrence identity plus optional
  parent region;
- `RegionBegin` / `RegionEnd`: explicit control-flow lifetime boundaries;
- `RegionStorage`: an allocation-site storage decision distinct from stack and
  heap;
- `RegionAllocate`: typed class or backing-store allocation associated with a
  region;
- `RegionCleanup`: nontrivial destruction/resource obligation;
- reference provenance: heap, stack owner, or a specific region;
- region escape/fallback reason;
- capacity/growth class: fixed, reusable fixed, or growable;
- teardown ordering metadata.

Pass interaction should follow the existing architecture:

1. ownership summaries describe parameter/return/store behavior without
   selecting storage;
2. escape analysis records where values leave lexical owners and candidate
   common boundaries;
3. a dedicated region-feasibility analysis proves graph membership and
   reference directions;
4. `StorageDecisionAnalyzer`, as sole storage authority, selects stack, region,
   or heap and records the reason;
5. ARC materialization uses reference provenance to produce region cleanup and
   ordinary heap ownership operations;
6. ownership transfer and borrow inference consume, but do not mutate, region
   decisions;
7. ARC optimization removes only operations justified by existing region and
   ownership facts;
8. performance validation consumes final capacity/storage/cleanup facts;
9. backends lower explicit region operations without redoing analysis.

The exact pass order may need a fixed point between graph feasibility and
storage selection, but the result must be an explicit deterministic analysis,
not backend heuristics. Phase 12.1B adds none of these structures.

## 19. Observability requirements

Future reports must distinguish static sites from runtime frequencies and
ordinary heap fallback from proven region storage.

Conceptual `toro inspect --memory` output:

```text
FrameEffect allocation
  storage: region
  region: simulate.tick
  lifetime: loop iteration
  capacity: growable
  cleanup: trivial
```

Conceptual `toro explain-memory` output:

```text
decision: region
why:
  escapes lexical construction through List<FrameEffect>
  does not escape loop-iteration region
  no weak observation or foreign escape
fallback if:
  list alias escapes tick
```

Reports should include:

- region identity and parent;
- lifetime boundary and all exit cleanup edges;
- allocation sites and concrete types;
- reference-direction proof/fallback reason;
- weak-target eligibility;
- capacity/growth and performance-contract classification;
- destructor-list membership and deterministic order;
- heap edges retained/released at teardown;
- static ARC operations removed because of region lifetime;
- possible contained and mixed strong cycles.

`MemoryReporter`, `MemoryExplainer`, and performance reporting remain read-only
consumers. They must not infer a region or change storage decisions.

## 20. Runtime implementation alternatives

| Runtime | Advantages | Costs/limits | Fit for measured case |
| --- | --- | --- | --- |
| fixed bump allocator | cheapest deterministic allocation/reset | fixed capacity and exhaustion contract | strong if effects bound is known before tick |
| chunked bump allocator | stable addresses and flexible capacity | general allocation on growth; retained chunks | strong normal-code prototype |
| destructor-list arena | preserves `destroy` and heap-field cleanup | registration and teardown can retain lifecycle cost | needed for nontrivial classes, unnecessary for trivial FrameEffect |
| reusable chunks | amortizes frame-to-frame allocation | retains peak storage; reset/stale-reference proof | especially strong for repeated ticks |

A future measured prototype should use aligned chunks, stable object addresses,
and reusable storage. Large allocations need a separately tracked chunk or
ordinary heap fallback selected before object identity is exposed. The runtime
must never relocate an allocated class object.

Destructor metadata should be sparse: register only objects or collection
resources with nontrivial cleanup. Registering every trivial effect would
retain avoidable per-object work and undermine the measured goal. Reusable
chunks should keep a bounded high-water mark visible to inspection; otherwise
flat allocator time may be purchased with unacceptable retained memory.

## 21. Risks

- region inference may be unstable across small source edits;
- conservative fallback may make expected performance disappear;
- heap-to-region or outer-to-inner mistakes create dangling references;
- mixed region/heap strong graphs complicate ARC materialization;
- destructor order may become observable in previously last-release-driven
  code;
- weak-target support can restore the control allocation A2 identified;
- growable chunks can hide general allocation inside a claimed bounded path;
- region growth can retain more peak memory than the current per-tick frees;
- collection aliases/clones can escape independently of their elements;
- generic and dynamic dispatch can widen the eligible graph unexpectedly;
- FFI escape cannot be recovered after a pointer is exposed;
- optimization-driven semantics must not make valid source depend on region
  selection;
- a solution tuned only to `FrameEffect` may not generalize to parser/request
  graphs.

## 22. Recommendation

```text
Prototype alternative first
```

Collection reservation and tick-list reuse are smaller, generally useful, and
can theoretically eliminate 26,000 baseline allocator invocations plus much of
the measured list-growth work. They do not eliminate the dominant 96,000 effect
object/control allocations or final lifecycle, so the experiment is a filter,
not a claim that arenas are unnecessary.

An arena prototype is not yet authorized because A2 did not isolate how much
of the combined sample share remains after list growth/object churn is removed,
and the initial safe region boundary deliberately excludes weak targets, FFI,
uncertain collection aliases, and concurrency. Measuring the simpler
alternative first keeps Toro's source model smaller and provides a clean
residual cost for a later prototype decision.

## 23. Proposed next experiment or milestone

Propose a separately authorized **Phase 12.1C collection reservation/reuse
experiment**:

1. add the smallest coherent `List<T>` capacity reservation and `clear`
   behavior consistent with current shared-reference semantics;
2. create an experimental simulation variant that allocates event/effect lists
   once, reserves their known capacities, and clears them each tick;
3. preserve effect destruction, checksums, native behavior, and A2 workload
   parameters;
4. repeat the A2 allocator accounting, seven-run timings, native sampling,
   peak-memory, and leak checks;
5. compare directly with the saved A2 baseline.

The experiment's expected accounting target is removal of up to 22,000
per-tick backing `realloc` calls and 4,000 per-tick list-object allocations.
It must not claim removal until measured.

The arena gate should advance to a narrow internal prototype only if the
post-alternative profile still shows `FrameEffect` object/control allocation
and final lifecycle as a material top hotspot. The pre-registered materiality
test is: the combined symbolized effect allocation plus teardown path remains
at least 10% of samples in repeated allocation-amplified profiles, with the
96,000 baseline object/control calls still present. If the path falls below
that threshold or becomes noise, defer arenas and prioritize collections.

If later authorized, the first arena prototype should be internal/test-only:

- one inferred lexical/loop-iteration region;
- single-threaded class allocation only;
- no source syntax;
- no escaping region references;
- no weak targets, FFI, `destroy()`, or heap-owning fields;
- trivial teardown plus reusable aligned chunks;
- existing heap fallback for every uncertain case;
- explicit IR decisions and inspection before backend lowering;
- differential correctness, strict C11, memory, and A2 profiling coverage.

That prototype remains pending both the collection experiment and separate
authorization.
