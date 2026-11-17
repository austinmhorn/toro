# Parser-tool string optimization report

## 1. Executive summary

The A3 tokenizer now scans UTF-8 bytes without allocating one-character
strings, and compiler-known native string producers return reclaimable Toro
string values rather than process-lifetime allocations. The 4,000-iteration
median fell from 2,148 ms to 809 ms for tokenization. The complete staged
median fell from 2,243 ms to 929 ms. Leak checking found zero leaked bytes and
a 4.0 MB peak physical footprint, versus the approximately 1.0 GB A3 peak.

## 2. A3 baseline

A3 measured 2,148 ms for tokenization and 95 ms for parse/analyze, or about
95.8% and 4.2% of 2,243 ms. Its shutdown-only native-string registry produced
approximately 1.0 GB peak physical footprint under `leaks --atExit` even though
the registry was ultimately freed and the leak total was zero.

## 3. Previous string representation

Toro `string` lowered directly to `const char*`. Literals and aliases were
shallow pointers. Compiler-known allocating helpers returned native buffers
without value-level ownership metadata.

## 4. Previous shutdown-only ownership model

Native string helpers linked allocated buffers into a process-wide list and
freed that list through `atexit`. This prevented dangling results, but a value
that became unreachable during execution could not be reclaimed before process
shutdown.

## 5. New owned/borrowed string model

Generated C uses a `toro_string` value containing data and an optional control
block. Literals and externally owned C strings have no control block. Values
adopted from compiler-known allocating helpers have a reference-counted control
block. Assignment, arguments, returns, fields, aggregates, and containers
retain and release those values without exposing ownership syntax in Toro.

## 6. `strings.byte_at` semantics

`byte_at(value, index) -> Result<int, string>` returns the unsigned byte at a
valid zero-based index. Negative, empty, equal-to-length, and greater-than-
length indexes return `error` with a static string. The success and error paths
therefore perform no string allocation.

## 7. UTF-8 byte boundary

The API exposes UTF-8 encoding bytes, not Unicode code points or grapheme
clusters. Tests cover ASCII letters, digits, whitespace, punctuation, and the
two bytes `195, 169` for UTF-8 `é`.

## 8. IR ownership integration

Typed IR records strings as `managed-string`. Existing ownership summaries,
borrow inference, and ARC optimization recognize managed strings where lifetime
facts apply, while escape analysis does not misclassify string values as class
allocation sites. The backend consumes these facts; it does not infer source
ownership policy from raw C pointer types.

## 9. Runtime/C representation

Static strings use a null control pointer. Compiler-known owned strings adopt a
buffer into a control block with a strong count. Final release frees both.
Generated entry wrappers assert that no compiler-owned native strings remain
live after normal entry completion, providing deterministic stress validation.

## 10. Aggregate/container ownership

Runtime helpers recursively retain and release owned strings in structs,
classes, enums, Results, `Array<string>`, `List<string>`, and
`Map<string,string>`. Class-field replacement releases the old value, and class
finalization releases inherited and direct owned fields through the established
lifecycle path.

## 11. Result/enum handling

Result propagation transfers the selected owned payload, releases the consumed
Result, and preserves early-error return ownership. `handle` temporaries and
case bindings remain scoped and reclaimable. Enum construction, assignment,
payload extraction, handling, and return use recursive lifetime helpers.
Collection-valued Result payloads remain deliberately rejected by the backend.

## 12. FFI boundary

Arbitrary `extern "C"` `c_string` values remain borrowed and externally owned.
Toro neither adopts nor frees them. The owned path is reserved for
compiler-known helpers whose ABI explicitly returns `toro_string`.

## 13. Tokenizer migration

Whitespace, punctuation, digits, identifiers, delimiters, and integer parsing
now classify byte integers. Only completed identifier, integer, and text token
lexemes are materialized with `substring`. The grammar and expected report are
unchanged; newline byte 10 now also advances line/column state correctly.

## 14. Correctness results

Focused tests cover bounds, UTF-8 bytes, aliases, reassignment, forwarding,
early returns, branches, loops, struct/class fields, arrays, lists, maps, enum
payloads, Result payloads, and 20,000 repeated native string creations. Parser
output remains byte-identical to `EXPECTED_OUTPUT.txt`. Full CTest and native
regressions pass.

## 15. Timing comparison

Five optimized samples measured tokenizer times of 822, 808, 802, 811, and
809 ms; parse/analyze times were 120, 123, 118, 120, and 119 ms. Medians are
809 ms and 120 ms, for a 929 ms staged total. Relative to A3, tokenizer time
dropped about 62.3% and total staged time about 58.6%. Millisecond readings are
coarse wall-clock observations, not a general benchmark claim.

## 16. CPU sampling comparison

The A3 sample showed substring, concatenation, classification comparisons, and
allocation. A two-second optimized sample no longer showed concatenation in the
hot path. It attributed most observed tokenizer samples to `strlen` reached by
`byte_at`, followed by the remaining token-lexeme `substring` calls,
classification, allocation/free, and string retain/release. Parsing and Map
analysis were not dominant during the tokenizer sample.

## 17. Allocation findings

The 2,256-byte stress fixture has 264 identifier/integer/text lexemes. The new
path therefore performs 264 lexeme substring materializations per tokenization,
or 1,056,000 across 4,000 iterations. A conservative lower bound for the old
path is one one-character substring per input byte plus one concatenation per
1,416 lexeme bytes: at least 3,672 allocating producer calls per scan, or
14,688,000 across the driver. Thus producer-call reduction is at least 92.8%;
the old implementation also repeated some character lookups, so its actual
count was higher. Retain/release activity remains visible but bounded.

## 18. Memory/high-water findings

`leaks --atExit` reported a 4,018 KB peak physical footprint for the optimized
4,000-iteration executable. A longer sampling driver reported a 2,018 KB peak
during observation. This replaces A3's approximately 1.0 GB instrumented peak
and confirms that unreachable compiler-known strings are reclaimed during
execution rather than accumulated for shutdown.

## 19. Leak findings

The final 4,000-iteration run reported 185 live allocation nodes totaling about
27 KB at inspection time and **0 leaks for 0 total leaked bytes**. The generated
entry invariant also reported zero compiler-owned native strings remaining
after normal completion.

## 20. Performance-contract behavior

`byte_at` is classified as `NativeCall` without `Allocate`; its bounds error
uses a static borrowed literal. A focused `performance function` wrapper passes
the existing contract and records its string parameter as an operation-bounded
borrow. No performance rule was weakened.

## 21. Remaining string limitations

There is no Unicode scalar/grapheme iterator, character primitive, slicing,
regex, interpolation, or source escape syntax. The new dominant tokenizer cost
is repeated length discovery (`strlen`) inside bounds-checked `byte_at`; a
length-carrying string representation or scan cursor is a future design issue,
not part of this milestone.

## 22. Arena evidence left deferred

The A3 AST remains five managed sites, zero Stack, zero Arena, and five Heap.
List storage remains conservatively escaping. No escape or arena rule was
changed to manufacture a storage result.

## 23. Phase 12.2 recommendation

**Defer.** Tokenization remains sequential and still accounts for about 87.1%
of staged time, while the application has no representative independent-file
parallel workload. The optimization supplies no new evidence for concurrency,
atomic ARC, or local/shared ARC specialization.

## 24. Roadmap implications

The immediate measured follow-up is efficient bounds-aware byte traversal that
does not rediscover string length on every access. Numeric parsing, source
escapes, Unicode-level APIs, collection-valued Result ownership, and broader
collection/arena reasoning remain separate future work. No next milestone is
authorized by this report.
