# Parser-tool length-aware string report

## 1. Executive summary

Toro's internal immutable string value now carries its UTF-8 byte length beside
the data pointer and optional ownership control block. The 4,000-iteration A3
profile measured a median 220 ms for tokenization and 123 ms for parse/analyze,
or 343 ms staged total. The preceding byte-at/ownership milestone measured
809/120/929 ms. CPU sampling found no `strlen` frames. Leak checking reported a
4,674 KB peak and zero leaked bytes.

## 2. Previous A3 baseline

Original A3 measured 2,148 ms tokenization, 95 ms parse/analyze, and 2,243 ms
total. Its allocation-heavy character traversal and shutdown-only string
retention reached approximately 1.0 GB in the instrumented run.

## 3. Previous byte-at optimization baseline

After nonallocating UTF-8 byte traversal and reclaimable compiler-owned strings,
five tokenizer samples were 822, 808, 802, 811, and 809 ms; parse/analyze was
120, 123, 118, 120, and 119 ms. Medians were 809 ms, 120 ms, and 929 ms total.
The remaining sampled hot path repeatedly called `strlen` for every bounds-
checked `byte_at`.

## 4. Prior runtime string representation

The prior generated `toro_string` descriptor held a `const char*` and an
optional reference-counted control block. Literals were borrowed. Buffers from
compiler-known native producers were owned and reclaimable. Length-sensitive
operations rediscovered byte length from the NUL terminator.

## 5. New length-aware representation

The descriptor now contains the data pointer, `size_t` byte length, and optional
control pointer. On the measured 64-bit host this grows a string value from 16
to 24 bytes. The ownership control block remains unchanged. Strings are
immutable, so aliases copy stable length metadata without synchronization or
mutation hazards.

## 6. Borrowed/owned behavior

Borrowed literals carry compile-time byte lengths and a null control pointer.
Owned strings carry their known output length and share the existing control
block across aliases. Assignment, reassignment, arguments, returns, fields,
enums, Results, and collections retain/release ownership exactly as before;
the copied descriptor preserves length unchanged.

## 7. FFI behavior

`c_string` remains a borrowed, immutable, externally owned NUL-terminated
pointer. Toro does not adopt or free it, and arbitrary reverse conversion to
`string` remains unavailable. At compiler-known host boundaries such as
environment and error copying, the runtime computes external length once,
copies the bytes, and adopts the resulting Toro-owned value with that length.

## 8. IR/backend integration

Typed IR continues to distinguish borrowed strings and `managed-string`
lifetime behavior without encoding a C struct layout. No semantic IR or effect
change was needed. The C backend materializes finalized string values as the
length-aware descriptor and consumes existing ownership decisions rather than
re-deriving provenance.

## 9. Standard-library integration

`fs.read_text` records the number of bytes read. External C copies scan once.
Substring and concatenation store their already-known output sizes. Static
errors and literals carry compile-time sizes. Filesystem writes consume stored
length rather than scanning their contents.

## 10. Byte-at behavior

`strings.byte_at(value, index)` checks the signed index against the cached byte
length and directly loads one unsigned byte. Success and bounds-error paths do
not allocate strings. Negative and out-of-range indexes preserve the existing
static borrowed error. The operation remains UTF-8 byte access, not Unicode
scalar or grapheme access.

## 11. Length behavior

`strings.length` now returns the cached byte length in O(1). Tests cover empty
strings, ASCII, the two-byte UTF-8 encoding of `é`, concatenation, substring,
aliases, returned strings, and aggregate/collection transport. Length and
`byte_at` observe the same byte model.

## 12. Equality, substring, and concatenation implications

Equality compares lengths before `memcmp`. Substring validates against cached
source length and adopts output with the requested count. Concatenation checks
size overflow, allocates `left + right + 1`, copies exact byte counts, writes the
terminator, and stores the summed length. Search and prefix/suffix checks also
use descriptor lengths. Map string hashing and equality consume cached lengths,
removing scans as a direct consequence rather than redesigning the map.

## 13. Correctness validation

Focused native tests cover literals, empty and owned strings, aliases,
reassignment, helper returns, struct/class fields, enum/Result payloads,
Array/List/Map storage, UTF-8 bytes, concat, substring, and computed map keys.
The existing 20,000-creation lifetime stress passes, and the parser report stays
byte-identical to `EXPECTED_OUTPUT.txt`. The clean build passed all 47 CTest
cases. Parser-project `check`, `fmt --check`, and native execution pass;
representative generated output compiles with `clang -std=c11 -pedantic-errors`.
Repeated generated C, typed IR, native report, and inspection output are
byte-identical.

## 14. Timing comparison

The five new tokenizer samples were 274, 226, 216, 216, and 220 ms. The first
sample includes cold-start variance; the median is 220 ms. Parse/analyze samples
were 129, 123, 123, 123, and 123 ms, with a 123 ms median. Median staged total
is 343 ms. Relative to original A3, tokenization is about 89.8% lower and staged
total about 84.7% lower. Relative to the prior follow-up, tokenization is about
72.8% lower and staged total about 63.1% lower.

## 15. CPU sample comparison

The prior optimized sample was dominated by `strlen` reached through
`byte_at`. A three-second, one-millisecond-interval sample of the new long driver
contained no `strlen` frame. Top-of-stack counts instead included tokenizer
work (330), identifier-start classification (295), `memmove` (209), native
`byte_at` (206), string release (161), allocation/free paths, the Toro
`byte_at` wrapper (134), and smaller substring/comparison activity.

## 16. `strlen` reduction evidence

Generated `toro_std_string_byte_at` and the `strings.length` lowering contain no
`strlen`; structural tests enforce that property. Within emitted runtime source,
`strlen` remains only at the conservative compiler-known external-C copy
boundary where no length is supplied. Literal, owned producer, equality, hash,
substring, concat, search, and file-write paths use stored length.

## 17. Allocation/memory findings

The representation change adds one `size_t` per string value but no allocation
per alias and no extra control allocation. The same 185 live allocation nodes,
about 27 KB at inspection time, remained bounded during the 4,000-iteration
instrumented workload. Compiler-known owned buffers are still reclaimed during
execution; there is no return to an `atexit` registry.

## 18. Leak findings

`leaks --atExit` reported **0 leaks for 0 leaked bytes**. The generated entry
invariant and 20,000-creation stress also completed without live compiler-owned
strings at normal exit.

## 19. Representation-cost tradeoffs

On this host, each copied string descriptor costs eight additional bytes (24
instead of 16). In exchange, length and bounds checks stop rescanning data, and
known producers avoid redundant work. Owned data/control storage is unchanged.
The measured footprint rose from 4,018 KB to 4,674 KB under `leaks`, still
bounded and far below the original approximately 1.0 GB behavior.

## 20. Remaining string limitations

Toro strings remain immutable and NUL-terminated at native boundaries. Source
syntax does not promise embedded NULs. Unicode scalar/grapheme iteration,
`char`, slices, interpolation, escapes, regex, mutable strings, ropes, SSO, and
interning remain out of scope. Arbitrary foreign pointers still provide no
trusted length or ownership.

## 21. New dominant runtime cost

No single replacement dominates as strongly as `strlen` did. The sample shows
tokenizer control/classification, `byte_at` Result/wrapper overhead, memory
moves, allocator/free activity for final lexeme materialization, and string
retain/release. These are evidence for later focused work, not authorization to
broaden this milestone.

## 22. Phase 12.2 recommendation

**Defer.** The new evidence is a sequential runtime representation win. It does
not establish a representative parallel workload or justify concurrency,
atomic ARC, or local/shared ARC specialization.

## 23. Roadmap implications

The specific repeated-length bottleneck is closed. Arena evidence remains five
managed AST sites, zero Stack, zero Arena, and five Heap; no placement analysis
changed. Future work may separately evaluate tokenizer call/Result overhead,
final lexeme allocation, Unicode APIs, source escapes, or collection-aware
storage, but none is authorized by this report.
