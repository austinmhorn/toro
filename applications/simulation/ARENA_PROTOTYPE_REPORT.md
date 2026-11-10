# Phase 12.1D — narrow internal arena prototype

## 1. Executive summary

The internal prototype eliminates the simulation baseline's 96,000 individual
`FrameEffect` object/control-block allocator calls while preserving byte-identical
output, class identity, virtual dispatch, deterministic lifecycle cleanup, and
zero-leak behavior. Generated allocator invocations fall from 96,272 after
12.1C to 272, a 99.72% reduction. An amplified native sample reduced the broad
effect storage/lifecycle path from approximately 35.5% to 11.7%.

The recommendation is **Proceed to language design**. This approves a later
design milestone, not public syntax or production-ready general arena support.
Phase 12.2 concurrency remains **Defer**.

## 2. Prototype scope

The experiment is enabled only by a non-stable `[experimental]` project-manifest
configuration naming `FrameEffect`, the `tick` region, 32 KiB of aligned
storage, and 256 teardown entries. Direct-file compilation and other projects
remain unchanged. This is not canonical Toro syntax or a general manifest API.

## 3. Region model

The storage analyzer identifies the configured class construction inside a
loop whose body clears every List receiving the constructed reference. The loop
body is the exact lexical region: its entry initializes fixed storage and its
normal exit performs teardown. Returns, `stop`, and `continue` make the body
ineligible in this prototype.

## 4. Eligibility and fallback rules

Stack promotion retains priority. Arena selection then requires a class of the
configured concrete type, only collection-storage escape causes, direct storage
into Lists proven cleared in the same region, no weak observation or weak field,
and no uncertain/foreign/field/return escape. An uncleared or additional
collection, unknown alias flow, persistent field, weak edge, FFI barrier, or
other uncertainty remains heap-backed. Heap is the conservative default.

## 5. IR and runtime implementation

Typed IR now represents `Arena` storage, region identity/provenance, fixed byte
and teardown capacities, allocation-site membership, teardown entries, ARC
suppressions, and prototype fallbacks. The C backend consumes these facts; it
does not infer eligibility. Generated C11 uses one aligned fixed buffer, one
fixed finalizer table, checked bump allocation, and deterministic abort on byte
or entry capacity exhaustion.

## 6. ARC behavior

Arena references do not use ordinary strong ARC for region-owned storage.
List insertion/clear and local aliases therefore do not free arena objects.
Arena objects still retain ordinary heap class fields, and teardown releases
those fields through the existing finalizer. Heap and stack ARC behavior is
unchanged. Performance contracts conservatively continue to treat arena
allocation as allocation.

## 7. Lifecycle and destruction behavior

Each arena object is registered once and finalized once in reverse allocation
order. Finalization preserves derived-to-base `destroy()` ordering, keeps
`self` and fields usable during destruction, releases heap-owned strong fields,
and invalidates its region-local control state. Neither object nor control
storage is individually freed; the bump offset is reset after all finalizers.

## 8. Collection interaction

`List<FrameEffect>` remains an ordinary reusable shared List. Its backing store
is reserved once and retained across ticks. `clear()` removes logical elements
without freeing arena storage; region teardown performs lifecycle cleanup.
Array, Map, and collection backing allocations were not moved into the arena.

## 9. Correctness comparison

Enabled and disabled focused runtime tests produce identical output, identity,
mutation, virtual dispatch, lifecycle counts, lifecycle order, derived/base
destruction, and heap-field cleanup. The simulation output is byte-identical to
`EXPECTED_OUTPUT.txt`. Capacity exhaustion fails deterministically rather than
overflowing. Weak-observed candidates remain heap-backed.

## 10. A2 baseline

| Metric | A2 |
| --- | ---: |
| malloc/calloc | 100,262 |
| realloc | 22,006 |
| total allocator invocations | 122,268 |
| FrameEffect object/control | 96,000 |
| sampled effect allocation + teardown | 36.9% |

## 11. 12.1C baseline

| Metric | 12.1C |
| --- | ---: |
| malloc/calloc | 96,264 |
| realloc | 8 |
| total allocator invocations | 96,272 |
| FrameEffect object/control | 96,000 |
| sampled effect allocation + teardown | 35.5% |

## 12. Arena allocator comparison

| Metric | A2 | 12.1C | Arena prototype |
| --- | ---: | ---: | ---: |
| malloc/calloc | 100,262 | 96,264 | 264 |
| realloc | 22,006 | 8 | 8 |
| total allocator invocations | 122,268 | 96,272 | 272 |
| FrameEffect object/control allocator calls | 96,000 | 96,000 | 0 |

The prototype still executes 96,000 object constructions and two checked bump
allocations per object, but neither reaches the general allocator. The 272
remaining generated allocator invocations belong to persistent objects and
ordinary collection storage.

## 13. CPU profile comparison

The 12.1C amplified sample had 501 application samples. The arena run had 445.
These are statistical inclusive groups, not cycle-accurate timings.

| Broad category | 12.1C | Arena prototype |
| --- | ---: | ---: |
| effect allocation | 12.0% | 7.0% bump allocation + 1.1% registration |
| effect clear/teardown/finalization | 23.6% | 3.6% |
| combined region storage/lifecycle | 35.5% | 11.7% |
| virtual actor update | 15.4% | 19.1% |
| event List insertion | 8.6% | 15.1% |
| event handling | 8.0% | 9.4% |

Allocator/free cost is removed; object initialization and required user/lifecycle
destruction remain. The relative share of unrelated work rises accordingly.

## 14. Timing comparison

Seven baseline arena runs measured four below 0.01 seconds and three at 0.01
seconds, a median below 0.01 seconds. The 12.1C median was 0.01 seconds. This
coarse timer supports no precise latency claim. An 8,000-tick run completed in
0.22 seconds wall time on the measurement host, including observed host noise.

## 15. Memory comparison

The seven-run baseline median maximum RSS was 1,130,496 bytes and median peak
footprint was 868,928 bytes, versus 12.1C's 1,064,960 and 803,328 bytes. The
roughly 64 KiB difference is consistent with the fixed aligned region and
teardown table plus host noise. The 8,000-tick run used 1,114,112 bytes maximum
RSS and 836,096 bytes peak footprint, demonstrating no per-tick growth.

## 16. Leak results

`leaks --atExit` reported 186 malloc nodes / 15 KiB at inspection and **0 leaks
for 0 total leaked bytes**. The normal macOS restricted-process warning was the
same as earlier studies. Region teardown, persistent heap cleanup, and process
cleanup all completed.

## 17. Observability examples

`toro inspect` and `toro inspect --memory` identify the `FrameEffect` site as
`storage arena`, include its deterministic `experimental.tick@...` region, and
state `general-heap-allocation no`. Static metrics report five managed sites:
zero stack, one arena, four heap; one teardown entry/site; two region ARC
operation suppressions; and zero simulation fallbacks. `toro explain-memory` reports
`decision arena-storage`; persistent Actor construction remains `heap-storage`.

## 18. Limitations

Eligibility is deliberately name/configuration driven and recognizes only the
direct cleared-List loop shape. It does not handle early exits, inferred general
regions, weak targets, FFI, cross-region references, collection backing storage,
threads, dynamic growth, runtime heap fallback, or arbitrary aliases. Capacity
exhaustion aborts deterministically. The configuration is experimental and
must not become a compatibility promise.

## 19. Semantic issues discovered

Region lifetime intentionally dominates local strong counts: logical List
clear does not destroy arena elements, and destruction occurs at region end in
reverse allocation order. This matches the approved prototype design but makes
destructor timing/order a real language-design question. The implementation
also had to prevent ownership-transfer analysis from treating arena locals as
ordinary owners. Persistent/uncertain collection destinations require explicit
proof, not merely the presence of some `clear()` in the loop.

## 20. Recommendation

```text
Proceed to language design
```

The prototype removes the targeted 96,000 allocator calls, materially reduces
the sampled lifecycle path, preserves the tested semantics, and keeps bounded
memory. A later design milestone may now specify whether and how Toro exposes
regions, destructor timing, capacity, diagnostics, and fallback. The prototype
itself is not production-ready and authorizes no public syntax.

```text
Phase 12.2 recommendation:
  Defer
```

No concurrency, atomic ARC, or local/shared ARC specialization evidence was
added by this experiment.

## 21. Phase 12.1E design follow-up

The authorized language-design milestone is now complete in
`docs/ARENA_LANGUAGE_DESIGN.md`. It selects the future `arena { ... }` lexical
lifetime-intent block with ordinary construction and compiler-selected
Stack/Arena/Heap placement. Its decision is **Authorize production
implementation** for a narrow Phase 12.1F slice.

That later design does not make this prototype's `[experimental]` manifest
configuration stable. The current compiler still does not accept arena source
syntax, and Phase 12.2 remains **Defer**.
