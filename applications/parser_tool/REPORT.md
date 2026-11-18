# Application Milestone A3 report

> This report preserves the original A3 baseline and findings. The authorized
> follow-up implemented UTF-8 byte scanning and reclaimable compiler-known
> native strings; its post-change measurements are in
> `STRING_OPTIMIZATION_REPORT.md`. The subsequent length-aware representation
> and final measurements are in `STRING_LENGTH_REPORT.md`.

## 1. Executive summary

Application Milestone A3 adds a multi-module parser/analyzer application under
`applications/parser_tool`. It reads a compact entity language, tokenizes it,
builds class-backed syntax nodes, resolves names with a map, validates links and
references, and emits a deterministic statistical report. The small, stress,
repeated, malformed, and duplicate-declaration fixtures all behave
deterministically.

The application exposed two compiler correctness defects, both fixed with
regressions: cross-module nominal identity was lost while substituting a
non-generic `Result` payload, and a collection returned inside `Result` could be
freed before use. The latter unsafe backend shape is now rejected explicitly;
the application uses a caller-owned output list.

Canonical `arena {}` is natural around one parse unit, but current analysis
places none of the five managed AST allocation sites in Arena: every node is
stored in a `List`, which is conservatively an escape. The workload therefore
provides useful outside-simulation evidence about the boundary, but no reason
to broaden arena semantics here. Tokenization accounts for about 95.8% of the
measured stage time and is dominated by allocation-heavy one-character string
operations. No material, measured parallel region was established.

**Phase 12.2 recommendation: Defer.**

## 2. Application purpose

The tool is a second substantial Toro application in a domain distinct from
the simulation. It exercises source loading, string scanning, typed tokens,
fallible parsing, object-graph construction, collection-backed storage,
symbol-table analysis, bounded parse-unit lifetimes, repeated processing, and
stable reporting without attempting compiler self-hosting.

## 3. Architecture

The executable follows a genuine staged pipeline:

```text
fixture file -> tokenize -> parse -> class-backed AST tables
             -> symbol/reference analysis -> AnalysisReport -> output
```

`runner` owns I/O and error presentation. `lexer` produces `Token` values into
a caller-owned list. `parser` creates entity, field, and link nodes inside one
lexical arena and invokes `analyzer` before leaving it. The report is a value
struct that outlives the parse-local collections. A separate `profile.toro`
entry point measures tokenization and parse/analyze stages without changing
the normal deterministic application.

## 4. Input language

The purpose-built grammar is:

```text
entity-declaration := entity Identifier { field* }
field              := Identifier : value
value              := Integer | [Text] | Identifier
link-declaration   := link Identifier -> Identifier
program            := (entity-declaration | link-declaration)*
```

An identifier value is an entity reference. Bracket-delimited text is used
because current Toro source strings have no escape syntax and the stdlib has no
character/code-point API suitable for matching a quote cleanly in this tool.
This is an application grammar choice, not new Toro syntax.

## 5. Module structure

- `src/main.toro`: deterministic fixture sequence.
- `parser_tool/model.toro`: token/value enums, token/report structs, AST classes.
- `parser_tool/lexer.toro`: scanning and integer parsing.
- `parser_tool/parser.toro`: syntax validation and AST construction.
- `parser_tool/analyzer.toro`: symbol table, references, counts, checksum.
- `parser_tool/report.toro`: stable report formatting.
- `parser_tool/runner.toro`: file loading and structured error output.
- `src/profile.toro`: repeated-stage profiling driver.

## 6. Toro features used

The application uses modules/imports, project manifests, public/private module
visibility, structs, classes, enums with payloads, exhaustive `handle`,
`Result<T,E>`, postfix `?`, nullable-free error paths, `List`, `Map`, `for` and
`while`, filesystem and I/O stdlib modules, lifecycle-safe class references,
`arena {}`, and five small `performance function` helpers. It does not force
inheritance, interfaces, weak references, or generic abstractions where they
would not improve this design.

## 7. Correctness fixtures

- `small.entity`: two entities, five fields, three value categories, one link.
- `stress.entity`: 24 entities, 72 fields, 24 references, and 24 links.
- `malformed.entity`: missing `:`, exercising a deterministic parse error.
- `invalid.entity`: duplicate declaration, exercising semantic failure.
- the small fixture is processed twice in one process to check repeatability.

`EXPECTED_OUTPUT.txt` records the exact normal output. The repeated small run
has the same counts and checksum as the first run. The stress result is 409
tokens with checksum `1423665770392516241`.

## 8. Parser/analyzer behavior

Tokens retain kind, lexeme, line, and column fields. Because current stdlib
string APIs expose substrings rather than character/code-point iteration and
Toro strings have no source escape for newline, this application currently
tracks an offset-like column and leaves line at one. Parsing validates the
grammar, numeric conversion, and EOF shape. Analysis rejects duplicate entity
names, unknown entity references, and unknown link endpoints, then computes
category counts and a deterministic traversal checksum.

AST identity is useful for entity, field, and link nodes, so they are classes.
Tokens and final reports are structs. `FieldValue` is a payload enum. Parallel
entity/field/link lists preserve the real graph relationship using field
ranges; collection-valued class fields are not currently backend-supported.

## 9. Arena behavior

The parser uses a canonical lexical `arena {}` around node construction,
analysis, and list cleanup. This is a natural compilation-unit lifetime. The
syntax causes no appreciable ergonomic friction and no nested arena is needed.

Nevertheless, every node is stored into a `List`. Escape analysis describes
each insertion as `collection-storage`, so storage decisions conservatively
fall back to Heap. The application did not change analysis or request
arena-backed collection buffers. Canonical arenas remain conceptually useful
outside simulation, but this common collection-backed AST shape receives no
runtime arena benefit today.

## 10. Stack/Arena/Heap findings

`toro inspect --memory` reports five managed allocation sites:

```text
total 5; Stack 0; Arena 0; Heap 5
arena runtime sites 0; teardown entries 0; ARC suppressions 0
heap fallbacks 5
```

Three sites construct `FieldNode`, one `EntityNode`, and one `LinkNode`.
All are within the parser arena region and all escape through collection
storage. `toro explain-memory` for `parser.toro:43:25` confirms heap storage,
the `List.add` escape cause, and retained collection ownership operations.

## 11. ARC/borrow/transfer findings

The final static memory report records 12 retains and 17 releases, unchanged by
ARC optimization, zero elided operations, zero proven ownership transfers, and
three inferred borrows. The borrows are operation-bounded initializer receiver
uses. Collection insertion and element cleanup retain and release node
references; analyzer parameters also carry conservative ownership traffic.
These facts match the Heap fallback rather than arena lifetime suppression.

## 12. Strong-cycle/weak findings

Inspection reports zero possible strong cycles and zero weak fields. The model
avoids parent back-references: nodes contain values and indices, and the parser
owns flat lists. Weak references would add no value to this graph. The current
model is natural for acyclic parser data, though richer parent-linked trees
would need deliberate weak back-references.

## 13. Performance-function findings

`token_kind_code`, `is_digit`, `is_identifier_start`,
`is_identifier_continue`, and `mix_checksum` satisfy their performance
contracts. The allocating tokenizer and parser are intentionally ordinary
functions. `toro ir --performance` therefore validates useful leaf helpers
without distorting the architecture to make the whole parser contract-safe.

## 14. String ergonomics at the A3 baseline

- Character iteration is absent, forcing allocating one-character
  `substring` calls: **architectural blocker** for a serious long-running
  parser workload.
- Runtime strings returned by native helpers stay valid until process exit;
  repeated substring/concat scanning produces a very high transient/retained
  footprint: **architectural blocker** for this workload class.
- Concatenating one character at a time allocates and dominates scanning:
  **significant ergonomic issue**.
- No standard numeric parsing API required a manual decimal parser:
  **significant ergonomic issue**.
- No string escapes/character literal made quote and newline recognition
  awkward, limiting source-location fidelity: **significant ergonomic issue**.
- String equality and length were straightforward: no material issue.

## 15. Collection ergonomics

`List.reserve`, indexed access, iteration, `clear`, and `Map<string,int>` are
enough for the pipeline. Reserving the token list avoids repeated backing-store
growth and clearing node lists makes parse-unit intent explicit. Map lookup is
clear for duplicate and reference validation.

The main limitation is that class fields containing collections remain an
unsupported backend shape, so the AST uses parallel lists and index ranges:
**significant ergonomic issue**. Also, storing class nodes in a list makes them
escape arena selection. Map iteration was not needed.

## 16. Standard-library gaps at the A3 baseline

`fs.read_text`, `io.write_line`, `strings.length/substring/concat`, and wall
time are sufficient for a bundled project. Missing character/code-point
iteration, owned/reclaimable temporary strings, integer parsing, richer
formatting, and monotonic timing are the important gaps. Process arguments are
not available, so the normal application uses fixed bundled fixtures rather
than accepting a command-line path. File I/O itself behaved correctly.

## 17. Compiler defects

Two correctness defects were isolated and covered by regressions:

1. Type substitution re-resolved an already-resolved non-generic nominal type
   in the caller module. A cross-module `Result<AnalysisReport,string>` then
   carried the wrong nominal identity in IR. `TypeChecker::substitute_type`
   now preserves the existing resolved type when no substitution is required.
2. Returning `Result<List<Token>,string>` could emit a collection pointer and
   release its local backing object before the caller retained it. Until the
   ABI can safely transfer reference-collection payloads, C generation now
   rejects List/Map Result payloads with a clear backend diagnostic instead of
   emitting use-after-free C. The app uses a caller-owned token list.

Neither fix adds a language feature.

## 18. Profiling methodology

After correctness, `profile.toro` read the stress fixture once, tokenized it
4,000 times, then parsed/analyzed a stable token list 4,000 times. Stage timing
used current `wall_millis`/`elapsed_millis`. External `sample` observed a native
profile executable for two seconds. `leaks --atExit` checked the same workload.
Inspection and all IR reporting modes supplied static allocation, effects,
ownership, escape, ARC, borrow, and performance evidence. Timings are coarse
single-machine observations, not benchmark claims.

## 19. CPU findings

One representative run measured 2,148 ms in tokenization and 95 ms in
parse/analyze: approximately 95.8% versus 4.2% of measured stage time. Sampling
occurred almost entirely during tokenization and showed `substring`, string
concatenation, the long identifier-classification comparison chain, and
allocation among visible hotspots. The analyzer's Map and traversal work was
not a dominant region at this scale.

## 20. Allocation/memory findings

`leaks --atExit` reported zero leaks and zero leaked bytes. It reported roughly
12.3 MB final physical footprint but an approximately 1.0 GB peak under the
instrumented repeated workload. A two-second sample observed about 621 MB
physical footprint while tokenization was active. The evidence is consistent
with allocation-heavy substring/concat and current process-lifetime retention
of the then-current native-returned strings, not with leaked AST nodes. Because
all five AST
sites are Heap, arena cleanup does not reduce that path's allocator/ARC work.

## 21. Scaling/repeated-run findings

Small and stress inputs, a repeated small parse in the normal executable, and
4,000 repeated stress-stage iterations all completed with stable checksums.
Logical AST cleanup is correct and leak checking is clean. Memory is not
practically bounded at a low high-water mark for repeated tokenization because,
at the A3 baseline, temporary native strings survived until process shutdown;
this was the primary scaling limit. The normal finite fixture run remained
small and deterministic.

## 22. Concurrency candidates

Independent input files would be the clearest future task boundary: each file
could own tokens, AST lists, symbol map, errors, and report, followed by a
deterministically ordered join. This application currently has one real input
at a time, so no speedup was measured and the 4,000 iterations are a profiling
loop rather than an application parallel workload.

Within one file, tokenization precedes parsing and consumes about 95.8% of the
measured stage time, so tokenization/parsing pipeline concurrency offers little
useful overlap. Top-level analysis is only about 4.2% together with parsing and
shares a symbol map; partitioning it would require ordering, deterministic
diagnostic reduction, cancellation/error policy, and synchronization for weak
likely benefit. Sensible scheduler granularity would be whole independent
files, not tokens, nodes, or fields.

## 23. Phase 12.2 recommendation

**Phase 12.2 recommendation: Defer.**

The only credible concurrency region is independent-file processing, and A3
contains no representative multi-file workload or measured native speedup.
Single-file dependencies are sequential, while shared symbol state makes the
small analysis phase unattractive. If a later workload establishes the need,
tokens/AST/Map state should remain task-local and only an immutable/value
`AnalysisReport` should cross the join. No current class reference needs to
cross an execution boundary, so A3 adds no evidence for local/shared ARC.

## 24. Roadmap implications

The highest-priority A3 implication was string/runtime work before concurrency:
nonallocating byte traversal plus reclaimable ownership for temporary native
strings. The authorized follow-up in section 25 completed that work. Standard
number parsing and source-location-friendly
character handling follow. Collection-valued class fields and collection-aware
arena escape/storage reasoning remain important application pressure, but A3
does not authorize either expansion. Phase 12.2 and local/shared ARC should
remain deferred until a representative multi-file workload demonstrates a
material parallel benefit.

## 25. Authorized string follow-up

The follow-up resolved the two highest-priority string findings without
changing source ownership semantics. `strings.byte_at` now supports
nonallocating UTF-8 byte classification, compiler-known native-produced strings
are reclaimed by value lifetime, and the tokenizer materializes only completed
token lexemes. Median tokenization fell from 2,148 ms to 809 ms. Instrumented
peak footprint fell from approximately 1.0 GB to 4,018 KB with zero leaked
bytes. Arena placement remains 0 Stack / 0 Arena / 5 Heap, and Phase 12.2
remains **Defer**. See `STRING_OPTIMIZATION_REPORT.md` for full methodology and
remaining limitations.

## 26. Length-aware string follow-up

The next evidence-driven follow-up added cached UTF-8 byte length to the
internal immutable string descriptor. `strings.length` and `strings.byte_at`
now obtain length in O(1), while literals and compiler-known producers preserve
lengths already known during construction. Median 4,000-iteration tokenization
fell from 809 ms to 220 ms; parse/analyze measured 123 ms, for a 343 ms staged
total. Leak checking reported a 4,674 KB peak and zero leaked bytes. Arena
placement remains 0 Stack / 0 Arena / 5 Heap, and Phase 12.2 remains **Defer**.
See `STRING_LENGTH_REPORT.md`.
