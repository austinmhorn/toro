# Phase 12.1C — collection reservation and reuse experiment

## 1. Executive summary

Phase 12.1C added only `List<T>.reserve(capacity)` and `List<T>.clear()`, then
used them to retain the simulation's event and effect lists across ticks. The
simulation output remains byte-identical to `EXPECTED_OUTPUT.txt`.

Deterministic generated-code accounting fell from 122,268 allocator
invocations in A2 to 96,272: 25,996 calls removed (21.26%). Reallocations fell
from 22,006 to 8, while all 96,000 `FrameEffect` object/control-block
allocations remained. In the allocation-amplified native sample, the combined
`FrameEffect` allocation and teardown path remained approximately 35.5% of
application samples, above the 10% gate registered by Phase 12.1B.

The experiment therefore recommends **Prototype arena**. This is evidence for
proposing a separate, narrow arena-prototype milestone; it does not authorize
arena syntax, IR, runtime support, or lowering here. Phase 12.2 concurrency
remains **Defer**.

## 2. A2 baseline

A2 used the same simulation, workload variants, native compiler pipeline, and
macOS profiling tools. Its deterministic generated-code accounting was:

| Metric | A2 baseline |
| --- | ---: |
| malloc/calloc calls | 100,262 |
| realloc calls | 22,006 |
| total allocator invocations | 122,268 |
| `FrameEffect` object/control allocations | 96,000 |
| per-tick List objects | 4,000 |
| per-tick List growth reallocations | 22,000 |

A2 attributed 13.6% of an allocation-amplified native sample to
`FrameEffect` allocation and 23.3% to effect release/finalization/free, or
36.9% combined. Event-list insertion/growth accounted for 10.7%.

## 3. List API changes

`list.reserve(capacity)` ensures room for at least `capacity` elements without
changing count or contents. It is a no-op when capacity is already sufficient,
may allocate otherwise, and rejects a negative capacity at runtime.

`list.clear()` releases every logical element, sets count to zero, and retains
the backing allocation for reuse. It performs no backing-storage allocation or
free. List assignment remains shared: aliases observe reserve/clear on the same
backing object. `clone()` remains independent.

For class references, nullable class references, class-backed interfaces, and
nested shared collections, clear uses the existing element-release path once
per stored element. Final destruction and weak invalidation therefore occur
normally. Primitive elements need no release.

## 4. Simulation changes

The simulation now creates its event and `FrameEffect` lists once, outside the
tick loop. It reserves the configured maximum event and effect counts, fills
the same lists each tick, consumes them, then clears both lists. Persistent
actor and summary collections are unchanged. The experiment does not reuse or
pool `FrameEffect` objects; clear releases every effect before the next tick.

## 5. Correctness validation

The project passed semantic checking and formatting checks. Native output was
compared byte-for-byte with `applications/simulation/EXPECTED_OUTPUT.txt` and
was unchanged. Focused tests cover aliases, clone independence, primitive and
managed elements, nullable references, generic managed elements, repeated
clear/reuse, weak invalidation, exactly-once destruction, retained capacity,
and negative reserve rejection.

## 6. Profiling methodology

The study repeated A2's small, baseline, tick-count, actor-count, effect-count,
and CPU-amplified variants. Each timing workload ran seven times under
`/usr/bin/time -lp`; medians use the same coarse 0.01-second resolution. Native
sampling used macOS `sample`. `MallocStackLogging` plus `malloc_history`
identified allocation sites; `vmmap`, `heap`, and `leaks` supplied live-memory
evidence. The latter tools require debugger-style process access and were run
under the same host permissions as A2.

Exact allocator totals below are deterministic counts of generated allocator
call sites multiplied by workload operations, as in A2. `malloc_history` was
used to validate stack identities and eliminated growth paths, not claimed as
an exact whole-process event counter. CPU samples are statistical, and live
memory snapshots are attachment-time observations.

## 7. Allocation comparison

| Metric | A2 baseline | 12.1C reserve/reuse | Change |
| --- | ---: | ---: | ---: |
| malloc/calloc calls | 100,262 | 96,264 | -3,998 (-3.99%) |
| realloc calls | 22,006 | 8 | -21,998 (-99.96%) |
| total allocator invocations | 122,268 | 96,272 | -25,996 (-21.26%) |
| tick List objects | 4,000 | 2 | -3,998 (-99.95%) |
| targeted tick-list growth | 22,000 | 2 reserves | -21,998 (-99.99%) |
| `FrameEffect` object/control | 96,000 | 96,000 | unchanged |

The eight remaining reallocations comprise the two explicit tick-list reserves
and six persistent-list growth calls. `malloc_history` showed one event reserve
and one effect reserve, no tick-list growth stack, and only persistent actor
growth through the ordinary add path.

## 8. CPU profile comparison

The allocation-amplified 12.1C sample contained 501 application samples:

| Broad category | A2 | 12.1C |
| --- | ---: | ---: |
| `FrameEffect` allocation | 13.6% | 12.0% |
| effect release/finalization/free | 23.3% | 23.6% |
| combined effect lifecycle | 36.9% | 35.5% |
| virtual actor update | 11.4% | 15.4% |
| event List insertion/growth | 10.7% | 8.6% insertion; no growth frames |
| event handling | 6.7% | 8.0% |

Sampling attribution moved slightly between runs, so the percentages are
directional rather than cycle-accurate. The controlled result is clear:
collection growth disappeared while the effect allocation/finalization path
remained a leading material category.

## 9. Timing comparison

Seven-run real-time medians after reuse were:

| Workload | 12.1C median real time |
| --- | ---: |
| small | <0.01 s |
| 500 ticks | <0.01 s |
| baseline | 0.01 s |
| 8,000 ticks | 0.04 s |
| 32 actors | <0.01 s |
| 512 actors | 0.02 s |
| 4 effects | <0.01 s |
| 96 effects | 0.02 s |

These medians are effectively unchanged from A2 at the timer's resolution.
One 96-effect run measured 0.06 seconds; the other six measured 0.02 seconds.
The study therefore makes no latency-speedup claim.

## 10. Memory/leak comparison

Median maximum RSS and peak physical-footprint results remained small and
bounded. The 12.1C baseline medians were 1,064,960 and 803,328 bytes,
respectively; the 8,000-tick medians were 1,310,720 and 1,049,152 bytes. A2
reported similarly flat values, with differences at this scale dominated by
host and snapshot noise.

In the amplified live snapshot, 12.1C reported approximately 1,249 KiB physical
footprint and 1,278 malloc-zone nodes / 92,032 bytes, versus A2's approximately
2,081 KiB and 1,391 nodes / 94 KiB. Attachment phase differs, so this is not a
causal memory-reduction claim. `leaks` again reported zero leaks and zero leaked
bytes; its process-debuggability warning matched A2.

## 11. FrameEffect remaining cost

Every tick still constructs fresh effect objects. The baseline workload
therefore still performs 48,000 `FrameEffect` constructions, 48,000 object
allocations, and 48,000 weak-control allocations. Clear removes the List's
strong references, after which the existing final-release path invalidates the
control block, runs lifecycle cleanup, and frees both allocations.

Those 96,000 allocations are now 99.7% of the 96,272 generated allocator
invocations. Their combined sampled allocation/teardown share remains 35.5%.
No object pooling, scalar replacement, arena storage, or lifecycle shortcut was
introduced.

## 12. Collection-specific improvement

Reservation and reuse achieved essentially the entire 26,000-call upper bound
identified by Phase 12.1B: 25,996 calls were removed. The four-call difference
is the two persistent reusable List objects and their two explicit reserve
operations. This confirms that reservation and clear are effective, general
collection mechanisms rather than an arena substitute hidden in the runtime.

## 13. Ergonomic findings

The two APIs are sufficient for predictable reusable scratch lists when an
upper bound is known. `clear()` expresses ownership cleanup and logical reuse
directly; `reserve()` avoids exposing allocator or exact-capacity details.
Shared aliases observing clear matches existing List identity semantics, while
clone remains the explicit independence operation.

The experiment does not justify broader collection additions such as remove,
pop, insert, public capacity, Map reservation, slices, or iterators. Selective
simulation entity removal remains a separate ergonomic gap.

## 14. Arena implications

The strongest measured non-arena alternative substantially removed the
collection churn it targeted. It did not reduce the bounded-lifetime object
path: effect allocation and final lifecycle stayed around 35.5% of application
samples, comfortably above the pre-registered 10% gate, with all 96,000
object/control allocations present.

That evidence supports proposing a separate narrow internal arena prototype
within the safety boundaries in `docs/ARENA_DESIGN.md`. It does not select an
arena syntax or authorize implementation. Any proposal must still preserve
identity, destruction, heap-field cleanup, weak/FFI exclusions, conservative
fallback, deterministic inspection, and current performance-contract policy.

Nothing in this profile demonstrates a material parallel speedup or changes
the Phase 12.2 concurrency evidence.

## 15. Recommendation

```text
Prototype arena
```

Reservation/reuse removed 21.26% of total generated allocator invocations and
virtually all tick-list growth, yet the bounded `FrameEffect` allocation and
teardown path remained material at approximately 35.5% of samples. The next
step may be a separately proposed and explicitly authorized narrow arena
prototype. No arena implementation is part of Phase 12.1C.

```text
Phase 12.2 recommendation:
  Defer
```
