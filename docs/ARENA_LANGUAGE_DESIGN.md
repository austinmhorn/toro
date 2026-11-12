# Phase 12.1E — arena language design specification

> **Design status:** approved and implemented for the narrow Phase 12.1F slice.
>
> The compiler accepts the canonical `arena { ... }` syntax. Phase 12.1D's
> manifest-controlled prototype has been retired; exclusions in this document
> remain authoritative for the initial production feature.

## 1. Executive summary

Toro should expose one new lifetime-intent construct:

```toro
arena {
    // ordinary Toro code
}
```

An `arena` block establishes a lexical lifetime boundary. Construction remains
ordinary source code. The programmer does not select an allocator, annotate
references, pass an arena value, or specify byte capacity. The compiler keeps
the existing storage priority:

```text
Stack -> Arena -> Heap
```

An object uses arena storage only when the compiler proves that every reference
is bounded by the block and that all required lifecycle behavior is preserved.
Failure to prove arena safety does not invalidate otherwise valid Toro: the
object remains stack- or heap-backed and inspection explains why. The block is
therefore a hybrid construct: its lexical boundary and cleanup are semantic,
while physical placement is compiler-selected and unobservable except through
performance and inspection.

The final decision is:

```text
Authorize production implementation
```

The authorized implementation milestone was the narrow Phase 12.1F slice
specified in section 31, and that milestone is now complete. Its authorization
does not extend to arbitrary arena values, manual reset, per-reference lifetime
syntax, weak arena targets, arena-backed collection buffers, FFI arena
references, concurrency, or performance-contract policy changes.

## 2. Evidence and motivation

The representative simulation established a bounded tick-local workload after
the existing ownership, escape, stack, transfer, borrow, and ARC passes:

| Metric | A2 | 12.1C | 12.1D prototype |
| --- | ---: | ---: | ---: |
| total allocator invocations | 122,268 | 96,272 | 272 |
| `FrameEffect` object/control allocator calls | 96,000 | 96,000 | 0 |
| sampled effect/region lifecycle path | 36.9% | 35.5% | 11.7% |

Phase 12.1C first removed collection churn with general `List.reserve` and
`List.clear` mechanisms. Phase 12.1D then isolated the remaining object path.
It preserved byte-identical output, identity, mutation, virtual dispatch,
exactly-once lifecycle, derived-to-base destruction, heap-field cleanup,
bounded memory, strict C11, conservative fallback, and zero reported leaks.

This evidence justifies a language design for common-lifetime class graphs. It
does not justify exposing the prototype's type-name manifest selector, fixed
32 KiB capacity, or 256-entry finalizer table as source semantics.

## 3. Design goals

- Express a bounded lifetime in one readable lexical construct.
- Preserve “ownership analysis, not ownership programming.”
- Keep class identity, mutation, dispatch, inheritance, interfaces, and ARC
  semantics independent of physical storage.
- Let ordinary construction participate without `arena Type(...)` syntax.
- Keep stack promotion ahead of arena placement and heap as the safe fallback.
- Make every placement and fallback deterministic and explainable.
- Preserve exactly-once lifecycle and all existing resource cleanup.
- Support the proven loop-iteration and temporary-graph shapes first.
- Leave a clean path to broader inference without requiring new source types.

## 4. Non-goals

The initial language feature does not provide:

- manual allocation/free or reset;
- named arena objects or allocator parameters;
- `owned`, `borrowed`, `move`, lifetime-variable, or arena-pointer types;
- arena-backed collection buffers;
- weak arena targets;
- managed references crossing C FFI;
- concurrency or cross-thread region sharing;
- a performance-contract exemption;
- object pooling, arbitrary placement, or a low-level memory allocator API;
- a guarantee that every construction in an `arena` block uses arena storage.

## 5. Terminology

- **`arena` block:** the user-facing lexical lifetime-intent construct.
- **arena lifetime:** one dynamic entry to an `arena` block through completion
  of cleanup on its exit.
- **logical arena object:** an object whose lifetime/storage analysis assigns it
  to that arena lifetime.
- **Arena storage:** the compiler storage classification distinct from Stack
  and Heap.
- **region:** the compiler/runtime implementation concept represented by a
  `RegionIdentity`; it is not a second source keyword.
- **arena fallback:** a candidate staying Stack or Heap because arena proof is
  absent or an unsupported relationship exists.

Toro uses `arena` as its one user-facing spelling. `region`, `pool`, and
`scope` are not aliases.

## 6. User-facing model

Programmers mark work that has an obvious common lifetime:

```toro
while tick < config.ticks {
    arena {
        effect := FrameEffect(position: position, ttl: 1)
        effects.add(effect)
        process(effects)
        effects.clear()
    }

    tick = tick + 1
}
```

Source references remain ordinary class references. No arena-specific type
appears in signatures or fields. Within the block, the compiler proves which
objects can share its lifetime. It may use stack storage for a smaller direct
local, Arena storage for a bounded graph, and Heap storage for an escaping or
unsupported object in the same block.

The block is not a promise that all allocations become cheap. It is a promise
that any storage assigned to its arena is finalized before the block finishes
and cannot be observed afterward.

### Source-model alternatives

| Model | Assessment | Decision |
| --- | --- | --- |
| Explicit lexical region | Makes the lifetime visible without naming references or allocators; maps directly to cleanup control flow. | Selected as `arena { ... }`. |
| Named lexical region | A name is useful only if constructions, references, or APIs name it too, which begins lifetime plumbing. Compiler-generated region identities already satisfy inspection needs. | Rejected for source. |
| Scoped arena value | Encourages capacity/reset methods and passing allocator values through APIs; lifetime can outlive obvious lexical structure. | Rejected. |
| Compiler-inferred region with opt-in hint | A lexical block is the cleanest form of this model: explicit lifetime intent, inferred placement. | Selected interpretation. |
| Fully inferred regions | Best source minimalism, but current proof boundaries and performance stability are not yet broad enough; small edits could silently create/remove a large optimization. | Deferred as a later optimization outside explicit blocks. |

An explicit `arena Type(...)` placement operator was also rejected. It would
make storage a property of each construction and force escape failures into
ordinary source, contrary to Toro's compiler-driven storage hierarchy.

### Design-only examples

The following examples specify future behavior. They are not accepted by the
current compiler.

Tick/frame lifetime, with reusable ordinary List backing:

```toro
while tick < ticks {
    arena {
        effect := FrameEffect(position: position, ttl: 1)
        effects.add(effect)
        update_effects(effects)
        effects.clear()
    }
    tick = tick + 1
}
```

Parser graph consumed within one parse unit:

```toro
arena {
    root := parse_syntax(source)
    validate(root)
    emit_diagnostics(root)
}
```

An escaping result remains heap-backed rather than making valid source fail:

```toro
function make_session() -> Session {
    arena {
        session := Session()
        return session // heap fallback: return outlives this arena
    }
}
```

A persistent store likewise prevents placement in the inner arena:

```toro
arena {
    enemy := Enemy()
    world.current_enemy = enemy // enemy is selected for Heap or an outer arena
}
```

Nested lifetimes permit inner-to-outer references:

```toro
arena {
    world := ScratchWorld()

    arena {
        effect := FrameEffect(world: world)
        process(effect)
    }
}
```

An arena object may own an ordinary heap object and releases it during logical
destruction:

```toro
resource := SharedResource()
arena {
    job := TemporaryJob(resource: resource)
    run(job)
}
```

Weak observation keeps the target heap-backed in the initial feature:

```toro
arena {
    actor := Actor()
    observer.target = actor // actor remains Heap; zeroing weak behavior is unchanged
    use(actor)
}
```

## 7. Canonical syntax and formatting

The one canonical syntax is an unnamed block statement:

```toro
arena {
    work()
}
```

There is no `arena name`, `arena(...)`, `arena Type(...)`, reset method, or
arena parameter syntax. Nested blocks use the same spelling.

The formatter places `arena` and `{` on one line, indents the body by four
spaces, and places `}` on its own line, exactly like `if`, `while`, and other
block statements.

## 8. Semantic versus optimization guarantees

`arena` is a hybrid:

- **Semantic:** it creates a lexical cleanup boundary. Any object assigned to
  that arena cannot outlive the dynamic block entry, and required cleanup runs
  on every exit.
- **Optimization:** the compiler chooses physical Stack, Arena, or Heap storage
  per allocation site. Source types and normal reference operations do not
  expose that choice.

The compiler must choose storage before an object's identity becomes visible.
It may not allocate an object in an arena and later move it to the heap. A proof
failure selects heap from the start.

Generated behavior must remain correct if an arena candidate falls back to
heap. Only allocation frequency, ARC traffic proven redundant, and storage
retention may differ. Lifecycle timing rules in section 14 apply regardless of
the physical representation selected for a logical arena object.

## 9. Lifetime rules

One dynamic entry to an `arena` block begins one arena lifetime. It ends after
the body and cleanup on every normal or abrupt language exit, including:

- fallthrough;
- `return`;
- `stop` and `continue` crossing the block;
- Result `?` propagation.

Compiler/runtime termination such as process abort does not promise user
lifecycle execution, matching existing process-failure behavior.

When an `arena` block executes inside a loop, every iteration creates a distinct
logical lifetime. The runtime may reuse physical chunks after completing the
previous iteration's cleanup. Reuse never joins the logical lifetimes and must
not make a stale reference valid.

A function activation is not implicitly an arena. Fully inferred regions may
be considered later, but the production feature begins with the visible block.

## 10. Allocation and storage-selection rules

For each construction associated with an active arena, the storage authority
applies:

1. select Stack when existing stack-promotion proof is sufficient;
2. otherwise select Arena when the whole relevant reference graph is bounded
   by that arena and supported by the runtime;
3. otherwise select Heap.

Ordinary construction syntax is sufficient. The initial candidates are class
objects, including concrete generic classes after substitution. Struct values
continue to use ordinary value placement.

Arena proof considers aliases, collection elements, strong fields, return and
payload flows, dynamic call targets, generic substitutions, FFI boundaries,
weak observation, and every control-flow exit. This checklist remains compiler
analysis, not source annotation vocabulary.

## 11. Escape behavior

An arena-stored reference never escapes its arena lifetime. Source that returns,
stores, or otherwise exposes an object beyond the block remains valid when
ordinary Toro typing permits it, but that allocation is selected as Heap (or a
compatible outer arena) before construction.

Examples that prevent placement in the current arena include:

```toro
return candidate
persistent.field = candidate
global_value = candidate
escaping_list.add(candidate)
foreign_call(candidate)
```

This is normally an inspection fact, not a warning or error. A hard compile
error is reserved for a future context that explicitly requires bounded arena
storage, such as a performance contract after that policy is defined.

## 12. Reference-direction rules

Rules are based on lifetime, not merely the word “heap”:

| Edge | Rule |
| --- | --- |
| same arena -> same arena | Allowed; region lifetime may replace per-edge storage ARC. |
| arena -> ordinary heap | Allowed; retain and release the heap target normally. |
| shorter local -> arena | Allowed while the local cannot outlive the arena. |
| longer-lived heap/outer owner -> arena | Not representable as an arena edge; choose compatible outer/heap storage for the candidate. |
| inner arena -> outer arena | Allowed because the target outlives the source. |
| outer arena -> inner arena | Prevents inner placement; select outer arena or heap before construction. |
| unrelated/sibling arena -> arena | Prevents arena placement without a proven lifetime ordering. |

A heap-allocated collection may temporarily contain arena references when the
compiler proves that all aliases and elements are removed or die no later than
the arena. Physical heap storage does not by itself imply a long lifetime.

## 13. ARC interaction

Arena storage does not replace ARC globally.

- Same-arena ownership edges may omit retain/release operations when region
  and logical-liveness proof make them redundant.
- Arena objects retain ordinary heap targets and release them at the object's
  logical destruction.
- Heap-only graphs use existing ARC unchanged.
- A longer-lived heap owner cannot retain an arena object; the candidate falls
  back before allocation.
- Borrow and ownership-transfer analysis consume the storage/provenance facts;
  they do not infer arena eligibility independently.

The initial implementation may conservatively retain some ARC within Arena
storage. Arena allocation is useful even when every ARC operation cannot yet be
removed. Correct lifecycle takes priority over suppression coverage.

## 14. Destruction semantics

Arena placement preserves Toro's exactly-once lifecycle model.

An arena object's storage remains reserved until arena teardown. Its logical
object lifetime may end earlier when the final logical strong ownership ends.
In that case `destroy()` and owned-field cleanup run at that point, while the
dead bytes remain unavailable until bulk reset. If ownership remains at arena
exit, teardown ends the lifetime and performs cleanup there.

For every object:

- `destroy()` runs at most once;
- `self` and fields remain usable during `destroy()`;
- class cleanup proceeds derived to base;
- strong heap fields release after the applicable `destroy()` body;
- weak slots held by the object are cleared normally;
- storage is not individually freed.

The first production slice may select Arena storage only when liveness ends at
the arena boundary, allowing it to implement this rule without new early-death
tracking. Early-death candidates remain heap-backed until that tracking exists.

## 15. Teardown order

Objects still alive at arena exit are destroyed in reverse semantic construction
order. This is a language guarantee for one arena lifetime. It is predictable,
works for cycles, matches the validated prototype, and avoids an ownership-graph
topological traversal.

An object already destroyed through earlier logical death is skipped. Within
one object, derived-to-base order remains independently guaranteed. Nested
arenas finish the inner teardown completely before the outer teardown resumes.

Code should not use peer objects after their logical destruction. Reverse order
does not promise that arbitrary peer graphs are all live during each destructor.

## 16. Weak-reference behavior

The initial feature keeps weak targets heap-backed. If an object may be weakly
observed, it is not stored in an arena. This is an explainable fallback, not an
error, and preserves Toro's zeroing guarantee without a second region-specific
weak-control model.

An arena object may hold a weak reference to an ordinary heap object; its weak
slot clears during normal object cleanup. Supporting weak arena targets later
requires separately validated control-block lifetime semantics and is not part
of Phase 12.1F.

## 17. Strong cycles

A fully arena-contained strong cycle does not retain storage beyond arena
teardown. It still has lifecycle order and mixed-edge implications. The initial
compiler keeps existing possible-cycle diagnostics unchanged rather than
silently changing warnings based on an optimization decision. Inspection may
add that the final selected graph is arena-contained and reclaimed at teardown.

Heap and mixed arena/heap cycles retain their existing ARC behavior and may
leak. Arena support is not a global cycle collector.

## 18. Collection behavior

Collection identity/backing storage and contained class objects are separate
decisions.

The initial feature arena-places eligible class objects only. `Array`, `List`,
and `Map` backing storage remains ordinary storage. A collection may hold an
arena reference only when the compiler proves all collection aliases and
elements are bounded by the same or a shorter lifetime.

- A collection declared wholly inside the arena can naturally be bounded by
  the block.
- An outer reusable List may hold arena objects only when every exit clears or
  otherwise removes those elements before teardown.
- An escaping alias or clone forces the referenced objects to compatible outer
  arena or heap storage.
- `clone()` never deep-copies class identity to manufacture safe escape.
- `clear()` ends logical element ownership normally; object destruction follows
  section 14 even though arena bytes remain reserved.

Arena-backed collection buffers, Map/Array region allocation, and collection
growth policy are later expansions requiring independent evidence.

## 19. Struct behavior

Ordinary structs remain values and receive no arena-specific syntax or identity.
They continue to use registers, stack storage, copying, and existing dynamic
backing representations. A struct containing a managed reference cannot hide
an escape: the referenced class object follows the same provenance rules.

Potential region placement of dynamically allocated struct/collection backing
storage is separate from this class-object feature.

## 20. Interface and inheritance behavior

Arena placement does not change object identity, base views, visibility,
virtual dispatch, overrides, or interface dispatch.

A class-backed interface value is a view of the same object and carries the
same lifetime provenance. Converting to an interface cannot hide an escape.
Every possible virtual or interface target must satisfy the lifetime proof;
uncertain or escaping targets cause fallback. Struct-backed interfaces retain
value semantics.

## 21. Generic, Result, and enum behavior

No arena-specific generic syntax exists. Analysis operates after concrete
substitution, so `Box<Particle>` and `Box<string>` may have different storage
outcomes while retaining their ordinary concrete identities.

Enums and `Result<T,E>` remain values. An arena reference in a payload carries
its lifetime restriction. Storing, returning, or propagating that value beyond
the arena causes the referenced allocation to use an outer arena or heap.
Postfix `?` is both a possible exit edge and a data-flow edge; cleanup runs
before propagation and any success/error payload escape participates in proof.

## 22. Function and module boundaries

An `arena` block is not an ABI promise and public signatures do not mention it.
Module identities and absolute paths have no effect on region identity.

The initial production slice handles allocations syntactically constructed in
the arena block. Later analysis may pass a hidden region context to a proven
callee or emit a region-aware specialization, including across modules. Such
propagation must be deterministic, visible in inspection, and semantically
equivalent to an ordinary call. Source APIs do not accept an arena parameter.

A callee return that outlives the block uses heap or a compatible outer arena.
Unknown, foreign, or unresolved call behavior prevents arena placement for
affected references.

## 23. FFI restrictions

Arena-backed managed class references do not cross C FFI. Current public FFI
already excludes Toro class references, so this preserves the safe baseline.
If future FFI accepts managed references, an unknown call is assumed to retain
or escape them and forces heap storage unless a separately designed no-escape
ABI contract proves otherwise.

This design adds no foreign lifetime annotation, arena pointer, or allocator
handle.

## 24. Performance-contract interaction

Current `performance function` policy does not change. Arena construction
continues to carry an allocation effect and is not discharged merely because it
uses bump storage.

A later policy milestone may permit arena allocation only when the finalized
runtime plan proves all of the following:

- storage exists before the contracted path or its creation is itself allowed;
- capacity and teardown entries are statically or configurationally bounded;
- allocation and exhaustion cannot invoke the general heap;
- no runtime heap fallback is possible;
- no hidden synchronization exists;
- teardown work is bounded.

Arena allocation and arena creation/growth are distinct effects. A growable
normal-code arena is not performance-contract safe merely because its fast path
is a pointer bump.

## 25. Capacity and failure model

Ordinary source does not specify bytes or finalizer counts. The normal-code
runtime uses aligned, stable-address, growable chunks and may retain/reuse a
bounded high-water set across repeated executions. Chunk growth is ordinary
general allocation and remains visible to effects/inspection. Objects are
never relocated.

Compile-time arena proof failure selects Heap. Runtime capacity exhaustion does
not switch individual objects to Heap because that would change provenance
after identity exists. A growable arena requests another chunk; if the system
cannot allocate it, execution follows Toro's existing process allocation-failure
behavior.

Fixed capacity and deterministic exhaustion are future performance-policy
concerns, not ordinary-program syntax. The prototype's 32 KiB and 256-entry
limits are not language defaults.

## 26. Nested arenas

Nesting is allowed:

```toro
arena {
    outer := Outer()

    arena {
        inner := Inner(parent: outer)
        use(inner)
    }
}
```

Inner objects may refer to outer or heap objects. Outer/heap objects may not
retain inner objects; such a flow causes the inner candidate to use the outer
arena when compatible or Heap. Sibling arenas have no lifetime ordering and
cannot retain one another's arena objects. Inner cleanup completes before outer
cleanup.

## 27. Concurrency boundary

The initial arena model is single-threaded. An arena-backed object, region
allocator, or reference may not cross a thread/task boundary. Because Toro has
no concurrency model yet, this creates no current source restriction beyond
the existing language.

Future concurrency design must define region ownership, task join lifetimes,
cross-thread references, synchronization, weak controls, and teardown
coordination before arenas can participate. Phase 12.2 remains **Defer**.

## 28. Diagnostics

Normal safe fallback is silent during compilation and explained on request.
It is not a warning: otherwise harmless code edits would create optimization
noise.

Future source errors include malformed `arena` syntax and any internal
inconsistency where generated control flow could leave an arena without cleanup.
A future policy requiring bounded storage should diagnose the relevant source
flow, for example:

```text
performance contract requires bounded arena storage
allocation remained heap-backed because value is returned from the arena block
```

Compiler-internal failures should identify the exact allocation, arena origin,
and unsupported edge rather than producing malformed C or a missing lookup.

## 29. Inspection and explanation behavior

The finalized storage decision remains observable. Conceptual output is:

```text
allocation FrameEffect @simulation.world:66:13
  storage arena
  arena simulate@58:5#iteration
  lifetime lexical-block
  general-heap-allocation no
  cleanup reverse-construction
  reason bounded by arena block and cleared List ownership
```

Fallback explanation is equally explicit:

```text
allocation Session @session.toro:20:5
  storage heap
  arena-candidate yes
  fallback returned beyond arena block
```

Reports distinguish static sites from runtime frequency and expose parent
arenas, capacity class, cleanup obligations, heap edges, ARC suppressions, and
fallback reasons. `MemoryReporter` and `MemoryExplainer` consume analyzer facts;
they never infer placement independently.

## 30. Experimental-prototype migration

Phase 12.1D's `[experimental]` manifest keys were never canonical and received
no compatibility guarantee. Phase 12.1F removed them from normal project
configuration after differential comparison. The completed migration:

1. migrated the simulation source to the canonical `arena {}` block;
2. removed the experimental type-name/region/capacity manifest entries;
3. stopped ordinary project configuration from accepting those entries;
4. replaced the fixed prototype runtime with growable production chunks; and
5. derives generated region identity from logical source/IR identity, never a
   machine path or manifest-selected type name.

There is no source migration alias and no permanent manifest arena API.

## 31. Production implementation stages

### Phase 12.1F — core lexical arena semantics

This was the exact authorized implementation milestone and is now complete:

- add the `arena { ... }` statement to lexer, parser, AST, formatter, semantic
  checking, and typed IR;
- create one logical arena lifetime per dynamic block entry and cleanup on all
  exit edges;
- preserve ordinary construction syntax and Stack -> Arena -> Heap selection;
- initially arena-place only class constructions syntactically inside the
  block whose complete ownership ends at block exit;
- support local aliases, init, methods, inheritance, virtual/interface views,
  `destroy()`, derived-to-base cleanup, and strong heap fields when existing
  analyses prove them safe;
- support class references in ordinary `List<T>` storage when the List or all
  of its elements are proven cleared on every arena exit;
- use Heap for return/global/persistent-field escape, weak targets, FFI or
  unknown calls, incompatible outer owners, early logical death not yet
  representable, or any uncertain proof;
- keep collection backing storage ordinary and do not arena-place structs;
- lower normal-code Arena storage to stable aligned growable chunks with sparse
  reverse-order cleanup metadata; no per-object runtime heap fallback;
- preserve exactly-once destruction and ordinary logical last-owner behavior;
- add deterministic inspection, memory metrics, explanation, and fallback
  reasons;
- replace/remove the experimental simulation manifest mechanism before marking
  the milestone complete;
- add arena-enabled/disabled differential tests, nested/exit cleanup tests,
  capacity-growth tests, strict-C11 and deterministic-emission tests, and all
  fallback/lifecycle/ARC/dispatch regressions;
- keep performance-contract policy unchanged.

### Later expansion, only with evidence

- hidden interprocedural arena-context propagation;
- earlier logical destruction/tombstone tracking for broader candidates;
- arena-backed collection buffers;
- weak arena targets;
- fully inferred arena scopes outside explicit blocks;
- bounded performance-contract arena support;
- FFI lifetime contracts;
- concurrency-aware regions.

## 32. Open questions

The following do not block the narrow production slice:

- whether retained growable chunks should use a global, function-local, or
  call-site-local high-water policy;
- whether later fully inferred regions should be reported as implicit arena
  blocks or a distinct optimization category;
- what evidence would justify arena-backed List/Map/Array buffers;
- whether weak arena targets are valuable enough to retain separate control
  storage;
- whether performance contracts need a canonical bounded-capacity declaration
  or can rely entirely on compiler-proven bounds;
- whether cycle warnings should eventually be downgraded for fully contained
  arena graphs;
- how future structured concurrency assigns and joins arena ownership.

None requires source ownership annotations in the initial model.

## 33. Final recommendation

```text
Authorize production implementation
```

The explicit lexical block provides predictable lifetime intent without
per-reference ownership programming. Ordinary construction plus compiler-driven
Stack/Arena/Heap selection preserves Toro's source model, while conservative
heap fallback keeps valid programs valid. The proven prototype demonstrates
material benefit and viable identity/lifecycle behavior. The narrow 12.1F
slice limits implementation risk to the measured class-in-cleared-List pattern
and closely related safe graphs.

Authorization is limited to section 31. It does not authorize broader arena
syntax, manual allocators, performance-policy changes, or concurrency.
