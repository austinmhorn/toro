# Arena feasibility and workload investigation

## 1. Executive summary

This document records the Phase 12.1 investigation into scoped or bulk
allocation. It does not specify or authorize arena syntax, lifetime syntax,
runtime APIs, IR operations, lowering, or a custom allocator ABI.

The investigation used a small compiler-development workload corpus written in
current Toro and the existing static inspection tools. Those workloads expose
seven heap-backed managed allocation sites whose values have a plausible common
function-bounded lifetime: three frame objects stored in a temporary list, two
parser nodes stored through strong fields, and two request values stored in a
temporary list. They demonstrate a real representational gap between ordinary
lexical stack promotion and aggregate-lifetime object graphs.

They do not establish that this gap is a material runtime cost. The corpus is
not a production benchmark, and Toro currently has no runtime allocation
profiler. Existing ownership transfer and ARC optimization already remove many
operations around these sites. The current evidence therefore supports keeping
arena design open, but not specifying or implementing it yet.

## 2. Current Toro memory baseline

Arena value must be measured against the current compiler, not against a model
where every reference is independently heap allocated and retained at every
use. Toro already provides:

- strong class references by default and explicit zeroing weak fields;
- a correctness-first non-atomic ARC fallback;
- explicit lifetime operations in typed IR;
- interprocedural ownership summaries;
- escape analysis with structured causes;
- stack promotion for proven non-escaping class constructions;
- final-use ownership transfers;
- operation-bounded borrow inference;
- ARC retain/release elision;
- advisory strong-cycle diagnostics;
- performance contracts evaluated against final storage decisions;
- `toro inspect`, `toro inspect --memory`, and location-specific
  `toro explain-memory`.

Heap storage remains the conservative fallback when an allocation escapes its
direct local owner, participates in weak observation, or cannot otherwise be
proved stack-safe. ARC remains the semantic lifetime mechanism for shared class
references.

## 3. Investigated workloads

The source corpus is
`investigations/arenas/workloads.toro`. It uses only current language features
and runs through the normal frontend, typed IR, analyses, C11 backend, and
native execution pipeline.

The corpus contains four patterns:

1. `frame_tick` constructs three `FrameParticle` objects, transfers them into a
   temporary `List<FrameParticle>`, observes the list, and drops it at the end
   of the tick function.
2. `parse_unit` constructs a three-node `SyntaxNode` chain. The root remains a
   direct local owner, while two children are stored through strong fields. The
   complete graph is discarded when the parse-unit function returns.
3. `handle_request` constructs two `RequestValue` objects, transfers them into
   a temporary list, and discards the list at request completion.
4. `make_session` returns a `Session`, representing an object whose lifetime is
   deliberately not bounded by its constructing function.

An audio/media graph was not added. Current Toro has no concurrency model or
owned buffer abstraction, and a single-threaded object list would duplicate the
frame/request shape without adding evidence.

The compiler reports an advisory possible cycle for the self-referential
`SyntaxNode.child` type topology. The concrete investigated chain is acyclic;
the warning remains useful because a parser graph could form a cycle.

## 4. Remaining heap behavior

The following figures come from `toro inspect --memory`; they count static
sites and IR operations, not runtime executions or elapsed time.

| Callable | Managed sites | Stack | Heap | Retains before/after | Releases before/after | Transfers |
|---|---:|---:|---:|---:|---:|---:|
| `frame_tick` | 3 | 0 | 3 | 3 / 0 | 7 / 4 | 3 |
| `parse_unit` | 3 | 1 | 2 | 2 / 1 | 4 / 3 | 1 |
| `handle_request` | 2 | 0 | 2 | 2 / 0 | 5 / 3 | 2 |
| `make_session` | 1 | 0 | 1 | 0 / 0 | 0 / 0 | 1 |
| Other lifecycle/main schedules | 0 | 0 | 0 | 0 / 0 | 2 / 2 | 0 |
| **Total** | **9** | **1** | **8** | **7 / 1** | **18 / 12** | **7** |

The frame and request objects escape through `collection-storage`. Their local
ownership transfers to collection elements, eliminating the corresponding
local cleanup releases and collection-entry retains, but the objects remain
heap backed and require collection-element cleanup.

The parser root is stack promoted because it remains a direct local owner. The
expression and literal nodes escape through `strong-field` storage and remain
heap backed. One child transfer is eliminated, while the first field insertion
still retains the new child and the local owner remains independently live.

The returned session escapes through `return` and transfers ownership to its
caller. Its lifetime is not a common bounded batch lifetime.

Representative explanations are:

```text
toro explain-memory investigations/arenas/workloads.toro:25:5
  FrameParticle: heap, escapes through collection-storage

toro explain-memory investigations/arenas/workloads.toro:36:5
  SyntaxNode: heap, escapes through strong-field

toro explain-memory investigations/arenas/workloads.toro:53:5
  Session: heap, escapes through return
```

## 5. Candidate arena use cases

### Frame/tick transient collection

The three particles cannot use ordinary stack storage because the list becomes
their owner. Their intended lifetime is the frame function, individual early
deallocation is unnecessary, and ownership transfer already proves that the
local names are dead after insertion.

Potential arena fit: strong in shape, unproven in performance impact.

### Parser/compiler batch graph

The child nodes outlive their construction expressions because strong fields
form a persistent graph. The graph is useful for one parse unit and can
conceptually die as a batch. Individual destruction order is not valuable for
these field-only nodes, although future nodes with `destroy()` would change
that conclusion.

Potential arena fit: strong in shape, with significant graph-safety and
destruction constraints.

### Request-scoped values

The request values are retained by a request-local list and discarded together.
They have the same static shape as frame transients.

Potential arena fit: plausible, but evidence from a real service and allocator
measurements is absent.

## 6. Cases where arenas do not help

- A returned object such as `Session` has an open-ended lifetime and is a poor
  region candidate unless a caller's larger lifetime can be proved.
- Independently shared objects with unrelated last-use points benefit from ARC's
  individual reclamation; a region could retain them longer.
- Long-lived mutable application entities do not naturally share one reset
  boundary.
- Strong cycles do not become semantically harmless merely because bulk release
  exists. Heap references into a finished region would still dangle, and
  finalizers could still own external resources.
- Weak-observed objects require control-block invalidation and weak-slot safety;
  a raw bulk free alone is insufficient.
- Large collection backing allocations may dominate costs even if their element
  objects are region allocated.

## 7. Alternatives considered

| Alternative | What it could address | Limitation relative to the candidates |
|---|---|---|
| Stronger escape analysis | More direct owners and aliases could become stack-backed | Does not naturally place a variable-sized object graph on one stack frame |
| Interprocedural stack/lifetime analysis | Could prove caller-bounded returned objects | Stack size and recursion make large batches risky |
| Scalar replacement | Removes short-lived objects whose identity is unobserved | Cannot generally replace aliased graph nodes or virtual/interface objects |
| Better collection allocation | Reduces backing-store growth and copies | Does not remove per-object heap storage for managed elements |
| Reusable buffers | Helps repeated frames, parsing, and media pipelines | Requires domain-specific data layout and reset discipline |
| Object pooling | Reuses objects across batches | Preserves per-object management complexity and stale-state risks |
| Compiler-inferred regions | Could capture common lifetime without source ownership syntax | Requires substantially stronger region, escape, and effect proofs |
| No change | Keeps the language and runtime simple | Leaves plausible batch-allocation costs when those costs become measurable |

The current corpus already shows that ownership transfer and ARC elision help:
six of seven retains and six of eighteen releases are absent from the final
plan. Arena evaluation must measure the residual cost rather than credit arenas
for work current analyses already remove.

## 8. Semantic constraints

### Lifetime safety

No arena-backed reference may outlive its region. The compiler would need to
reject or conservatively heap-fallback any return, heap-field store, escaping
interface wrapper, collection share, foreign call, or other path whose lifetime
cannot be proved within the region. Existing escape and ownership summaries are
a foundation, but they currently classify stack eligibility and do not prove a
shared region boundary for arbitrary graphs.

### Class/reference semantics

Storage choice must not change class identity. Aliases must observe the same
object; inheritance layout, derived-to-base views, virtual dispatch, interface
dispatch, nullable references, and concrete generic identity must remain
unchanged. Region storage cannot silently turn a class into a copied value.

### Destruction

Toro promises automatic, exactly-once `destroy()` and derived-to-base cleanup.
A future region could:

- record objects with lifecycle or owned-resource cleanup in a destructor list,
  invoke deterministic destruction, then free storage in bulk; or
- restrict eligible objects to those for which destruction can be omitted
  without changing observable behavior.

Calling every destructor preserves semantics but adds metadata and teardown
work. Restricting eligibility simplifies the allocator but creates potentially
surprising fallback boundaries. Plain bulk free without either rule is invalid.

### Reference directions

- **region to same region:** structurally natural, provided teardown handles
  destruction and does not run per-edge ARC unnecessarily.
- **region to normal heap:** safe only if the region retains heap targets and
  releases them during teardown.
- **normal heap to region:** unsafe unless the heap owner is proved not to
  outlive the region. Conservative fallback or rejection is required.
- **one region to another:** requires a proven nesting/order relationship;
  unrelated regions cannot safely retain each other by default.

### Generics and dispatch

Concrete specialization identity and managed-reference behavior must be
preserved. Generic construction cannot assume region eligibility merely because
one instantiation is safe. Virtual/interface target sets must all respect the
same lifetime boundary.

### FFI

Foreign code must not retain a pointer to region-backed storage beyond a proven
call boundary. Current FFI types do not expose class references, which is a safe
baseline. Any future expansion must preserve that restriction unless an ABI can
express and enforce the lifetime.

## 9. Runtime design options

| Strategy | Advantages | Costs and risks |
|---|---|---|
| Bump allocator | Minimal fast-path work and compact metadata | Fixed capacity or growth policy, alignment handling, no individual reuse |
| Chunked bump allocator | Grows without relocating objects; natural reset boundary | Chunk overhead, large-object policy, deterministic ordering requirements |
| Resettable reusable region | Amortizes allocations across repeated frames/requests | Retained peak memory, explicit reset safety, stale reference risk |
| Destructor list plus bulk free | Preserves lifecycle and owned-field cleanup | Per-object registration, reverse-order policy, teardown latency |

Any design needs defined alignment, overflow/failure behavior, handling for
allocations larger than a normal chunk, nested-region teardown order, and a
deterministic memory-reuse policy. A chunked bump allocator plus destructor list
appears capable of preserving current semantics, but no runtime strategy is
selected by this investigation.

## 10. Interaction with weak references

Zeroing weak references currently rely on a control block that outlives object
storage until all weak slots are cleared. Region teardown would need to
invalidate every target before freeing object storage while retaining control
blocks needed by weak observers outside the region. Alternatively, eligibility
could exclude weak-observed objects or require all weak slots to share the same
bounded lifetime.

This is not equivalent to inferred borrowing: `weak` is an explicit persistent
observation with nullable reads. Region storage must preserve zeroing behavior.
The current weak-control model makes unrestricted arena participation
substantially more complex than plain bump allocation.

## 11. Interaction with collections

The candidates rely on current collection ownership, so element storage and
backing storage must be considered separately:

- `Array<T>` owns contiguous value storage and copies independently. Region
  placement might help large bounded arrays, but copy semantics must remain.
- `List<T>` and `Map<string,V>` have shared ARC-managed backing storage. Placing
  that backing in a region conflicts with aliases that may outlive the region.
- Managed elements could be region backed while collection backing remains
  ordinary heap storage, but a shared collection escaping the region would then
  create dangling elements.
- Clone operations require an explicit policy: cloning into ordinary heap
  storage is safe but potentially expensive; cloning within a region requires a
  proven destination lifetime.

No uniform "collections use the arena" rule is safe without alias and lifetime
proofs.

## 12. Interaction with performance contracts

Current policy distinguishes stack-promoted construction from general heap
allocation. It has no arena category, and this investigation does not change
that policy. Any current heap-backed candidate continues to violate a
`performance function` allocation constraint.

A future policy could treat bounded bump allocation separately from general
heap allocation, but only after the runtime establishes bounded allocation
cost, growth/failure behavior, teardown cost, and destructor behavior. A region
that may grow through the general allocator or run an unbounded destructor list
cannot automatically satisfy a predictable-execution contract.

## 13. Compatibility with Toro philosophy

Three broad approaches were considered without defining syntax:

### Explicit arena object

This is predictable and lets APIs name a destination allocator, but it exposes
allocation plumbing throughout ordinary code and risks becoming ownership
programming.

### Scoped region construct

A lexical scope gives a visible lifetime boundary and deterministic teardown.
It is simpler to reason about than a free-standing allocator, but references
crossing the scope boundary still require compile-time enforcement and the
construct is new source-level lifetime syntax.

### Compiler-inferred region

Inference best matches “ownership analysis, not ownership programming” and can
preserve unchanged source. It is also the most difficult approach: the compiler
must discover common lifetimes, keep decisions predictable, explain fallback,
and avoid unstable performance changes across small edits.

If arenas are eventually justified, inference with conservative heap fallback
appears most compatible with Toro's philosophy. A limited lexical opt-in could
be considered only if real workloads show inference cannot express a necessary
boundary predictably. This is a compatibility assessment, not a design choice.

## 14. Risks

- Dangling heap-to-region or cross-region references.
- Observable destruction order changes or skipped finalizers.
- Weak control blocks surviving object storage incorrectly.
- Memory retained until region teardown even when objects die early.
- Peak-memory regressions from reusable chunks.
- Hidden general-heap growth presented as predictable arena allocation.
- Complex fallback rules that make performance hard to understand.
- Interaction bugs across inheritance, interfaces, generics, collections,
  nullable references, and FFI.
- Pressure to introduce source ownership/lifetime programming contrary to Toro's
  goals.
- Optimizing static sites that are cold while adding runtime complexity to all
  programs.

## 15. Proposed next step

Do not create an arena implementation milestone yet. Reopen arena design when a
representative Toro application—such as a real frame loop, parser/compiler, or
request service—shows repeated bounded-lifetime heap allocation after the
current optimization pipeline and profiling shows allocator, ARC, destruction,
or fragmentation cost is material.

The evidence needed is:

- `inspect --memory` and `explain-memory` records identifying the residual
  candidate sites and their common lifetime pattern;
- runtime allocation counts/bytes and time attributable to those sites from an
  independently approved profiling effort;
- evidence that stack promotion, scalar replacement, collection improvements,
  or reusable-buffer design cannot adequately address the cost;
- at least one lifecycle/weak/collection case establishing the semantic surface
  a future design must preserve.

If that evidence appears, the next arena-specific milestone should specify the
source model or inference boundary, lifetime proof, permitted reference
directions, destruction and weak behavior, collection policy, FFI restrictions,
IR representation, diagnostics/inspection, and performance-contract treatment
before implementing runtime storage.

## 16. Recommendation

**Defer.** Arenas plausibly fit bounded frame, parse-unit, and request graphs,
but static site evidence alone does not demonstrate a material runtime problem.
Gather application and profiling evidence before specifying or implementing an
arena feature.

## 17. Application Milestone A2 update

The external profiling study in
`applications/simulation/PROFILE_REPORT.md` satisfies the runtime-cost part of
the reopening gate for one representative workload. Its bounded tick-local
`FrameEffect` path accounts for 78.5% of generated baseline allocator calls;
effect allocation and final lifecycle together account for approximately 36.9%
of samples in an allocation-amplified native profile. Existing transfer,
borrowing, and ARC-elision decisions do not remove that heap lifecycle.

The updated recommendation is **Reopen** for a separately approved design and
alternatives investigation. This does not select an arena model and does not
authorize syntax, lifetime annotations, IR operations, runtime allocation, or
backend lowering. Collection reuse/reservation, value representation, scalar
replacement, and collection-aware storage must be evaluated alongside any
region design. Phase 12.2 concurrency remains deferred.

## 18. Phase 12.1B design outcome

The authorized design and alternatives investigation is complete in
`docs/ARENA_DESIGN.md`. It specifies a preferred eventual inferred,
lexically anchored, hybrid-ownership region model, but recommends:

```text
Prototype alternative first
```

Reservation plus reuse of the event/effect lists could theoretically remove
26,000 baseline allocator invocations without new lifetime semantics. It cannot
remove the 96,000 `FrameEffect` object/control allocations, so the experiment
is intended to isolate the residual cost rather than assume arenas are
unnecessary.

No arena implementation is authorized. The next proposed activity is a
separately authorized collection reservation/reuse experiment using the saved
A2 workloads and profiling method. An internal arena prototype remains
conditional on its result and separate approval. Phase 12.2 remains deferred.
