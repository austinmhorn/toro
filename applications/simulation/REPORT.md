# Application Milestone A1 — representative simulation workload

## 1. Application purpose

This project is a deterministic, single-threaded game/simulation-style Toro
application. It exists to exercise the current compiler as an integrated
product and to collect evidence for future memory, concurrency, collection,
performance-contract, standard-library, and ergonomics decisions.

It is not a benchmark suite, an arena prototype, or a concurrency design.

## 2. Architecture

The application advances a fixed world for a configured number of ticks.
Persistent actors move through a bounded two-dimensional space. Each tick also
creates a list of short-lived effects and a list of data-carrying events. Event
handling updates deterministic counters and a bounded checksum.

The project contains six Toro source modules:

| Module | Responsibility |
| --- | --- |
| `main` | lifecycle probe, verification and stress runs |
| `simulation.config` | validated value-type configuration |
| `simulation.math` | non-allocating hot numeric routines |
| `simulation.entities` | actors, inheritance, virtual updates and weak references |
| `simulation.events` | data-carrying events and exhaustive handling |
| `simulation.world` | persistent world state, tick-local state and statistics |

The manifest uses the normal project workflow with `src/main.toro` as its
entry. Public declarations cross module boundaries; helpers such as
`create_actors` and `simulate` remain private to `simulation.world`.

## 3. Toro features exercised

- project manifests, dotted imports and private-by-default modules;
- value structs for vectors, configuration, event totals and final statistics;
- identity-bearing classes for actors and effects;
- class inheritance and virtual override dispatch through `Actor` references;
- one class initializer with private field state;
- one meaningful `destroy()` lifecycle observation;
- strong references in persistent and temporary lists;
- nullable weak references for leaders and lifecycle observation;
- `Array<int>`, `List<T>` and `Map<string, int>`;
- a data-carrying enum with exhaustive `handle`;
- `Result<T, E>` construction, handling and postfix `?` propagation;
- concrete generic collection instances;
- two non-allocating `performance function` hot helpers;
- loops, comparisons, arithmetic, member mutation and deterministic output.

## 4. Application size and workloads

The application contains 298 lines and about 8 KiB of Toro source across six
modules, excluding this report and the manifest.

Two fixed workloads are executed:

| Workload | Ticks | Persistent actors | Effects per tick | Runtime effect constructions | Handled events |
| --- | ---: | ---: | ---: | ---: | ---: |
| verification | 12 | 8 | 3 | 36 | 120 |
| stress | 2,000 | 128 | 24 | 48,000 | 260,000 |

The parameters are ordinary function arguments, so the scale is easy to edit
in source. Current lack of process argument-vector access prevents selecting a
scale from the command line without changing the program.

## 5. Correctness and output

Repeated native runs produced byte-identical standard output:

```text
lifecycle actor destroyed
weak reference cleared
run
small
ticks
12
actors remaining
8
events processed
120
effects spawned
36
effects expired
36
team zero score
31284
team one score
35384
checksum
9708436005
run
stress
ticks
2000
actors remaining
128
events processed
260000
effects spawned
48000
effects expired
48000
team zero score
192377299
team one score
192509251
checksum
254399
```

`EXPECTED_OUTPUT.txt` stores this result for direct regression comparison.

The lifecycle line appears exactly once. The following weak-reference check
confirms that the returned observer was zeroed after its target's final strong
release.

## 6. Memory behavior

`toro inspect --memory` reports static compiler facts, not runtime frequencies:

```text
managed-allocation-sites total 5 stack 0 heap 5
escape-sites non-escaping 0 escaping 4 unknown 1
static-arc-retains 4 -> 1
static-arc-releases 14 -> 11
elided-static-arc-operations 6
static-ownership-transfers 4
static-inferred-borrows 13
possible-strong-cycles 0
weak-fields 2
```

Persistent actor constructions escape through `List<Actor>` storage. The
tick-local `FrameEffect` construction also escapes through
`List<FrameEffect>` storage. The observer escapes through return, while its
lifecycle target remains conservatively unknown because it is passed into a
weak-field construction.

Collection backing allocations are not counted as managed class-construction
sites in this summary. The application creates new event and effect lists on
every tick, so native profiling must account for those runtime allocations as
well as the 48,000 stress-run effect objects.

## 7. Stack versus heap findings

No managed class construction in this application is stack promoted.

- Persistent actors correctly need storage beyond `create_actors` and remain
  heap-backed through collection escape.
- Frame effects are logically bounded to one tick, but collection insertion
  makes the current allocation site escape and therefore heap-backed.
- The returned observer must remain heap-backed.
- The lifecycle target is kept on the heap because escape analysis is
  conservative at its weak-field constructor argument.

`toro explain-memory src/simulation/world.toro:64:13` identifies the frame
effect's collection storage as the decisive heap reason. This is the most
important residual bounded-lifetime allocation site in A1.

## 8. ARC, borrowing and ownership transfer

The optimized static schedule reduces retains from four to one and releases
from fourteen to eleven. Four ownership transfers are recorded:

- newly created scouts transfer directly into persistent actor storage;
- the actor list transfers through the function return;
- newly created frame effects transfer directly into the tick-local list;
- the returned observer construction transfers to its caller.

The frame-effect transfer removes both the collection retain and the local
cleanup release at that source site. Receiver borrows are inferred for actor
virtual updates, effect updates, the effect expiration query, lifecycle code,
and other observation-only methods. Thirteen borrows are present in the final
static report.

The remaining ARC plan is understandable from `toro ir --arc` and
`toro ir --ownership`; no source ownership annotations were needed.

## 9. `performance function` findings

`integrate_axis` and `mix_checksum` are genuine hot numeric helpers. Both
contracts are satisfied with no additional proof requirement. They allocate
nothing, perform no I/O, and call no unknown code.

The full tick routine is intentionally not marked `performance function`. It
constructs growable collections and heap-backed effects, so marking it would
misrepresent the current execution plan rather than improve the application.

## 10. Strong cycles and weak references

`Scout.leader` is weak because the actor list owns both the leader and scouts;
the back-reference is observational. `ActorObserver.actor` is also weak and
demonstrates zeroing after the target is destroyed.

The memory report finds two weak fields and no possible strong cycles. The
`LifecycleActor` finalizer executes once before its weak observer is checked.
The persistent actor hierarchy is destroyed through the existing most-derived
lifecycle path when each simulation run releases its actor list.

## 11. Compiler defects discovered

No compiler correctness defect was found.

Clang reports a non-fatal generated-C readability warning for an equality
condition with redundant parentheses. Strict C11 compilation still succeeds,
and behavior is correct. This is a backend code-quality nuisance rather than a
correctness failure, so A1 does not change the compiler for it.

## 12. Language ergonomics issues

1. **Significant ergonomic issue:** `List<T>` has no removal, pop, clear, or
   capacity-reservation operation. The simulation cannot naturally model
   selective entity/projectile removal or reuse per-tick list capacity. It
   instead drops complete tick-local lists.
2. **Nuisance:** there is no remainder operator in the current arithmetic
   subset, so bounded coordinate/checksum logic uses explicit branches.
3. **Nuisance:** no string interpolation or multi-value formatted output exists;
   the summary uses separate label and value lines.
4. **Nuisance:** workload sizes cannot be selected from process arguments.
5. **Documented backend limitation:** giving the inheritance base an `init`
   would require unsupported base-initializer chaining. `Actor` therefore uses
   field-based construction, while the independent `FrameEffect` naturally
   exercises `init`.

None of these prevents the current workload, but collection removal and reuse
would materially improve a larger simulation.

## 13. Standard-library gaps

- process argument-vector access for workload selection;
- monotonic duration and lightweight profiling APIs;
- richer collection mutation and capacity control;
- basic numeric helpers and deterministic pseudo-random generation;
- formatted output/string building.

A1 does not expand the standard library. The application uses fixed inputs and
external host timing instead.

## 14. Runtime and performance observations

The stress run completes successfully as a native executable. One informal
host measurement reported approximately 0.15 seconds wall time for the complete
program, including both verification and stress runs. This single observation
is not a benchmark and does not attribute time to allocation, ARC, dispatch, or
simulation arithmetic.

Repeated generated C and inspection output are byte-identical. The generated
C compiles as strict C11 with `-pedantic-errors`.

Meaningful cost attribution was not available from Toro tooling during A1.
External allocation and CPU profiling was therefore required before drawing
runtime optimization conclusions; A2 later performed that study.

## 15. Potential concurrency opportunities

The application remains entirely single-threaded.

### Actor updates

- Work: update each actor's position and compute an update value.
- Reads: immutable tick configuration and each actor's own velocity/state.
- Mutates: each actor's own position; shared event and team-score accumulation
  currently happens in the same loop.
- Shared references: the actor list and weak leader references. The leader is
  not read by the update method today.
- Required synchronization: none for disjoint actor state if results are written
  to per-worker buffers; deterministic merging would be required for events and
  scores.
- Ordering: final event order and score/checksum reduction order must be fixed
  to preserve deterministic output.
- Errors/cancellation: current updates cannot fail, so cancellation is not
  required.
- Likely benefit: possible only at much larger actor counts; A1 provides no
  profiling evidence that task overhead would pay for itself.

### Frame effects

- Work: decrement independent effect lifetimes and calculate checksum inputs.
- Reads/mutates: each effect is independent after list construction.
- Shared references: the immutable list view and final reduction only.
- Required synchronization: per-worker reductions followed by deterministic
  ordered combination.
- Ordering: checksum combination is order-sensitive.
- Errors/cancellation: none.
- Likely benefit: the current 24 effects per tick are far too small to justify
  parallel scheduling.

### Event handling

Event totals can be reduced in batches, but ordered `handle` processing is cheap
and deterministic. No current evidence supports parallelizing it.

These regions suggest that deterministic data-parallel operations and structured
join/reduction semantics would be more relevant than unstructured shared-memory
threads. They do not yet demonstrate a need to design either model.

## 16. Arena evidence

A1 strengthens the *shape* evidence from Phase 12.1: the real update loop
constructs 48,000 `FrameEffect` objects in the stress configuration, each
bounded to one tick, and creates tick-local list backing storage for events and
effects on all 2,000 ticks. The effect allocation remains heap-backed solely
because it is stored in a collection. Its local-to-collection ownership transfer
already removes a retain/release pair.

This is a credible region-shaped workload, not proof of an arena performance
problem. There is no allocation-time, ARC-time, destruction-time, peak-memory,
or fragmentation profile. Collection reuse, capacity reservation, value-type
effects, scalar replacement, or stronger collection-aware lifetime analysis may
address some or all of the same cost with less semantic complexity.

**Phase 12.1 arena recommendation after A1: Defer.** The application strengthens
candidate-site evidence but does not satisfy the required runtime-cost gate.

## 17. Recommended roadmap implications

```text
Phase 12.2 recommendation:
  Defer
```

A1 identifies potentially parallel actor/effect loops but does not show that
concurrency is necessary or beneficial. It supplies no scheduler, contention,
latency, or scaling evidence and therefore does not justify concurrency syntax,
atomic ARC, or local/shared ARC specialization.

The smallest legitimate next activity identified by A1 was a separately
authorized external profiling study of this application. A2 completed that
study without changing Toro semantics. A second representative application,
especially a parser/compiler-style tool, would still test whether the observed
collection and bounded-lifetime patterns generalize.

## 18. A2 profiling supplement

Application Milestone A2 completed the external study proposed above. Its
canonical evidence is in `PROFILE_REPORT.md`, with compact raw summaries under
`profiling/`.

The baseline executes 48,000 tick-local `FrameEffect` constructions and 96,000
corresponding object/control-block allocator calls. In an allocation-amplified
native sample, effect allocation sites accounted for 13.6% of samples and
effect release/finalization/free for 23.3%. Peak memory remained flat as tick
count increased, and `leaks` reported zero leaks. Event-list growth was a
secondary visible cost at 10.7% of samples.

This runtime evidence changes the arena gate to **Reopen** for a future design
and alternatives investigation. It does not authorize implementation. Phase
12.2 remains **Defer** because no parallel speedup or material parallel
bottleneck was demonstrated.
