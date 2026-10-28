# Application Milestone A2 — external simulation profiling study

## 1. Executive summary

A2 profiles the unchanged A1 simulation algorithm through native executables
built by Toro. The representative 2,000-tick workload finishes in a median
0.01 seconds on this host, and the 8,000-tick workload in 0.04 seconds. Peak
resident memory stays near 1.1 MiB across the tick scaling series, so the
tick-local state is being released rather than accumulated.

An allocation-amplified native sample provides the main actionable evidence.
Of 682 symbolized samples, 159 (23.3%) were in `FrameEffect` list teardown,
release, finalization, or free, and 93 (13.6%) were at its two allocation sites.
Event-list insertion/growth accounted for 73 samples (10.7%). The bounded
tick-local object lifecycle is therefore a material cost in this workload,
not merely a static candidate. This is sufficient to reopen an arena *design
investigation*, but not to authorize arena syntax or implementation.

The profile does not justify concurrency. Actor virtual updates accounted for
78 samples (11.4%), while allocation, teardown, and serial collection work
dominated. No parallel speedup, scheduler cost, contention, or synchronization
measurement exists.

## 2. Profiling environment

- Apple arm64 host, Darwin 24.5.0;
- macOS 15.5 (24F74);
- Apple clang 17.0.0 (`clang-1700.0.13.5`);
- system C compiler selected by the normal `toro build` command;
- `/usr/bin/time`, `/usr/bin/sample`, `/usr/bin/leaks`, `/usr/bin/heap`,
  `/usr/bin/vmmap`, and `/usr/bin/malloc_history` available;
- `xctrace` unavailable through the active developer tools installation.

No profiling package was installed and no Toro runtime instrumentation was
added.

## 3. Compiler, version, and commit

- compiler output: `toro compiler v0.1.0`;
- source commit: `e6bcde92fb84c9c26a84ed2dad664aba20d85f3d`;
- commit description: `e6bcde9 Add representative simulation application`;
- generated backend language: C11.

The profiling variants are uncommitted A2 application inputs on top of that
commit. No compiler or runtime source changed during the study.

## 4. Build procedure

Each executable was produced through the ordinary Toro pipeline, for example:

```bash
./build/toro build applications/simulation/src/profile_baseline.toro \
  -o /tmp/toro-a2-baseline
```

The compiler loaded the project manifest and modules normally, emitted C, and
invoked the host C compiler. Generated C was not hand-edited or optimized.
`profile.toro` only suppresses the verbose summary and prints the deterministic
checksum; it calls the same `run_simulation` function as A1.

## 5. Workloads

| Name | Ticks | Actors | Effects/tick | Effect constructions | Purpose |
| --- | ---: | ---: | ---: | ---: | --- |
| small | 12 | 8 | 3 | 36 | verification/tool sanity |
| ticks 500 | 500 | 128 | 24 | 12,000 | tick scaling |
| baseline | 2,000 | 128 | 24 | 48,000 | A1 stress case |
| ticks 8,000 | 8,000 | 128 | 24 | 192,000 | tick scaling |
| actors 32 | 2,000 | 32 | 24 | 48,000 | actor scaling |
| actors 512 | 2,000 | 512 | 24 | 48,000 | actor scaling |
| effects 4 | 2,000 | 128 | 4 | 8,000 | effect scaling |
| effects 96 | 2,000 | 128 | 96 | 192,000 | effect scaling |
| CPU sample | 50,000 | 512 | 96 | 4,800,000 | obtain a one-second native sample |

Only one dimension changes within each scaling series. The amplified CPU
workload is not presented as representative latency; it exists because the
normal stress case is shorter than a useful sampling interval.

## 6. Correctness validation

Every workload completed successfully and produced a stable checksum:

| Workload | Checksum |
| --- | ---: |
| small | 9,708,436,005 |
| ticks 500 | 515,575,571 |
| baseline | 254,399 |
| ticks 8,000 | 432,143 |
| actors 32 | 108,943 |
| actors 512 | 928,683 |
| effects 4 | 253,159 |
| effects 96 | 190,973 |
| CPU sample | 1,545,740,399 |

The original project entry still matches `EXPECTED_OUTPUT.txt`. Repeated runs
of each profile executable produced its recorded checksum.

## 7. Timing methodology

Each executable received one unrecorded warm-up followed by seven independent
runs under `/usr/bin/time -lp`:

```bash
/tmp/toro-a2-baseline >/tmp/toro-a2-output \
  2>/dev/null
/usr/bin/time -lp /tmp/toro-a2-baseline \
  >/tmp/toro-a2-output 2>/tmp/toro-a2-time.txt
```

`time` reports only hundredth-second resolution on this host. Values below
0.01 seconds therefore appear as `0.00`; the report does not interpolate false
precision. The raw samples are in `profiling/timing.tsv`.

## 8. Timing results

Wall samples, in seconds:

| Workload | Seven samples | Median | Min–max |
| --- | --- | ---: | ---: |
| small | 0, 0, 0, 0, 0, 0, 0 | <0.01 | <0.01 |
| ticks 500 | 0, 0, 0.01, 0, 0, 0, 0 | <0.01 | <0.01–0.01 |
| baseline | 0.01 × 7 | 0.01 | 0.01–0.01 |
| ticks 8,000 | 0.04 × 7 | 0.04 | 0.04–0.04 |
| actors 32 | 0 × 7 | <0.01 | <0.01 |
| actors 512 | 0.02 × 7 | 0.02 | 0.02–0.02 |
| effects 4 | 0 × 7 | <0.01 | <0.01 |
| effects 96 | 0.02 × 7 | 0.02 | 0.02–0.02 |

User time tracked wall time for all resolved samples. The amplified CPU run
took 1.11 seconds wall and 0.95 seconds user in a separate sanity measurement.

## 9. CPU profile findings

The following command collected 682 main-thread samples at a 1 ms interval:

```bash
/tmp/toro-a2-cpu >/tmp/toro-a2-cpu.out &
/usr/bin/sample "$!" 1 -file /tmp/toro-a2-sample.txt
```

| Symbolized category | Samples | Share |
| --- | ---: | ---: |
| FrameEffect list/object release, finalization, free | 159 | 23.3% |
| FrameEffect object/control allocation sites | 93 | 13.6% |
| virtual actor update, including numeric helper | 78 | 11.4% |
| event List add/growth | 73 | 10.7% |
| event handling | 46 | 6.7% |
| Array get | 44 | 6.5% |
| event List get | 36 | 5.3% |
| actor List get | 29 | 4.3% |
| Array set | 24 | 3.5% |
| FrameEffect List add/growth | 9 | 1.3% |

These are inclusive sample groups, not cycle-accurate measurements. The raw
stack report had enough native symbol information to identify the generated
helpers directly. `profiling/cpu-summary.txt` preserves the compact evidence.

## 10. Allocation findings

The baseline executes 122,268 allocator invocations emitted by the generated
application path. This is deterministic accounting of executed `malloc`,
`calloc`, and growth `realloc` calls, checked against `malloc_history` stack
identities:

| Source | Calls | Share |
| --- | ---: | ---: |
| 48,000 FrameEffects: object + weak control | 96,000 | 78.5% |
| event List growth | 14,000 | 11.5% |
| FrameEffect List growth | 8,000 | 6.5% |
| per-tick event/effect List objects | 4,000 | 3.3% |
| persistent actors/controls/collections | 268 | 0.2% |

The total comprises 100,262 `malloc`/`calloc` calls and 22,006 `realloc`
growth calls. A `realloc` may grow in place, so “allocator invocation” is more
precise than claiming every call creates a new block. The count excludes
allocator work internal to process startup and libc.
`malloc_history` confirmed the two distinct effect allocation stacks and the
collection growth stacks, but attach overhead made it unsuitable for an exact
whole-run counter. The exact call total therefore comes from deterministic
runtime loop counts applied to the emitted allocator calls, not from confusing
the five static managed-allocation sites with execution frequency.

## 11. Peak-memory findings

Median maximum resident set size remained essentially flat:

| Series point | Median max RSS | Median peak footprint |
| --- | ---: | ---: |
| 500 ticks | 1.25 MiB | 1.02 MiB |
| 2,000 ticks | 1.06 MiB | 0.81 MiB |
| 8,000 ticks | 1.06 MiB | 0.83 MiB |
| 32 actors | 1.05 MiB | 0.80 MiB |
| 128 actors | 1.06 MiB | 0.81 MiB |
| 512 actors | 1.12 MiB | 0.88 MiB |
| 4 effects/tick | 1.06 MiB | 0.81 MiB |
| 24 effects/tick | 1.06 MiB | 0.81 MiB |
| 96 effects/tick | 1.06 MiB | 0.81 MiB |

One 500-tick run reached 1.75 MiB RSS; it did not repeat and is retained in the
raw table as host noise. A live `vmmap` snapshot during the amplified workload
reported a 2,081 KiB physical footprint. `heap` reported 1,391 malloc-zone
allocations using 94 KiB. Memory tracks live actors and one tick's temporary
state, not cumulative ticks. There is no sign of unbounded tick-local growth.

## 12. Leak findings

The baseline was checked with:

```bash
/usr/bin/leaks --atExit -- /tmp/toro-a2-baseline
```

`leaks` reported 186 nodes malloced for 15 KiB at inspection and **0 leaks for
0 total leaked bytes**. It also warned that the process was not debuggable, so
restricted memory contents could only be read from read-only regions. The
allocation/leak totals were still reported. No strong cycle is intentionally
created, matching the A1 static cycle result.

## 13. ARC/runtime-helper findings

Residual ARC/lifecycle work is visible and material for `FrameEffect`:
23.3% of samples were under list release, object release, finalization, and
allocator free. A1's ownership transfer already removes the insertion retain
and local release, so this residual work is the necessary final owner release
under the current heap-backed representation. Actor retain/release was not a
major hot path in the sample.

Static ARC operation counts remain useful compiler-plan facts, but the 23.3%
sample share is the runtime evidence used here.

## 14. FrameEffect findings

- baseline constructions: 48,000; amplified profile: 4,800,000;
- two allocator calls per construction: object plus zeroing-weak control;
- baseline effect allocation calls: 96,000, or 78.5% of generated calls;
- allocation sites: 13.6% of CPU samples;
- teardown/release/finalization/free: 23.3% of CPU samples;
- lifetime: all effect objects are released with their list at tick end;
- memory: flat tick-scaling RSS demonstrates reuse/release rather than growth.

The candidate materially affects CPU and allocator traffic but not peak memory.
Current transfer/borrow/ARC optimization cannot eliminate its allocation or
final destruction because collection storage makes it escape.

## 15. Collection findings

Collections account for 26,000 baseline allocator calls: two list objects per
tick plus 11 backing reallocations per tick. Event-list insertion/growth is a
visible 10.7% CPU category; effect-list growth is 1.3%. Map and Array access is
visible, especially Array get/set, but Map work was not a major sampled hotspot.

Object allocation and lifecycle are larger than collection growth in the
amplified profile. Even so, capacity reservation or tick-list reuse could
remove substantial calls without new lifetime syntax and should be compared
against any future arena prototype.

## 16. Scaling observations

- **Ticks:** roughly linear above timer resolution: 2,000 ticks take 0.01 s and
  8,000 take 0.04 s; peak memory is flat.
- **Actors:** broadly linear but coarsely measured: 32 is below resolution,
  128 takes 0.01 s, and 512 takes 0.02 s. Fixed per-tick effect work prevents a
  clean fourfold ratio.
- **Effects per tick:** broadly linear but coarsely measured: 4 is below
  resolution, 24 takes 0.01 s, and 96 takes 0.02 s. Fixed actor work likewise
  dilutes the ratio.
- No superlinear trend or tick-dependent memory growth was observed.

## 17. Concurrency opportunities

Actor updates are independent at the object-mutation level and could use
per-worker events/scores followed by a deterministic ordered reduction. They
represent only 11.4% of the amplified profile. Effect updates are also
independent, but their current cost is dominated by serial allocation and
teardown, and the checksum is order-sensitive. Event processing mutates shared
totals and must preserve deterministic ordering.

This identifies possible parallel regions, not a demonstrated parallelism
problem. No multi-core scaling, scheduler overhead, contention, cancellation,
or atomic-lifetime measurement exists. Reducing allocator/collection overhead
is the evidence-backed priority before concurrency design.

## 18. Arena evidence

A2 satisfies the Phase 12.1 reopening criterion narrowly: a representative
application has a repeated, bounded, compiler-visible lifetime whose current
heap allocation plus final lifecycle consumes a material sampled share. The
effect path accounts for 78.5% of generated allocator calls and approximately
36.9% of samples across its allocation sites and teardown path.

Reopening means comparing designs and alternatives. It does **not** authorize
arena syntax, lifetime annotations, IR operations, or runtime support. Any next
design study must compare collection reuse/reservation, value representation,
scalar replacement, and collection-aware storage with region allocation.

## 19. Tooling limitations

- `/usr/bin/time` resolution is 0.01 seconds, so small cases are censored.
- `sample` gives statistical shares from one allocation-amplified workload,
  not exact per-function time for the baseline.
- `xctrace` was unavailable in the selected developer tools installation.
- `malloc_history` attachment greatly perturbed execution and was useful for
  stack attribution, not whole-run counts.
- `leaks`, `heap`, and `vmmap` required host process-inspection permission.
- RSS includes loader/runtime state and exhibits small host-level noise.
- no fragmentation, cache-miss, energy, or multi-core speedup data was gathered.

Temporary raw reports and binaries lived under `/tmp/toro-a2-*` and were
removed after the compact summaries were recorded.

## 20. Recommended roadmap implications

1. Reopen Phase 12.1 as a design/measurement investigation, beginning with an
   alternatives comparison rather than syntax or implementation.
2. Prioritize collection capacity reservation/reuse and measure it against the
   same workload; event/effect list growth is a demonstrated secondary cost.
3. Preserve existing ownership transfer and borrow analysis; it successfully
   removes intermediate ARC but cannot remove final heap lifecycle here.
4. Keep Phase 12.2 deferred until a post-allocation profile shows a substantial
   parallel region and a prototype can measure speedup and determinism costs.
5. Improve benchmark tooling or use a higher-resolution external timer before
   making claims about sub-10-ms application latency.

No compiler/runtime correctness defect was found.

## 21. Final arena recommendation

```text
Arena recommendation:
  Reopen
```

Runtime evidence now demonstrates material repeated bounded-lifetime
allocation/lifecycle cost that current transfer, borrow, and ARC-elision passes
do not address. This is authorization to reopen investigation only.

## 22. Final Phase 12.2 recommendation

```text
Phase 12.2 recommendation:
  Defer
```

The study identifies theoretically independent work but not a material
parallel bottleneck or measured benefit. Concurrency, atomic ARC, and
local/shared ARC specialization remain unauthorized.
