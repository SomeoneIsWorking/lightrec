# Project state

Comparison baseline: upstream Lightrec at
`550f700c037e8713e4567227514424621cd39cd7`, whose default configuration
interprets every cold block while compiling in the background.

| ID | Capability | State | Evidence or gap |
|---|---|---|---|
| LR-EXEC-1 | Cold blocks compile synchronously before execution | verified | Clang-built x86_64 runtime test executes a cold two-instruction block as one JIT block with zero fallback. |
| LR-EXEC-2 | Typed, measured fallback | verified | Runtime falsifiers cover unsupported control flow and unsafe fetch with per-reason block/instruction totals; diagnostic interpretation leaves fallback telemetry untouched. |
| LR-EXEC-3 | Dynarec-dominated conformance decision | partial | Strict-majority API implemented; consumer integration remains. |
| LR-EXEC-4 | Exact runtime block-boundary interception | verified | Clang-built runtime tests prove callbacks run before cold lookup, on a cache hit, and at a direct jump target; stop leaves the exact PC unexecuted, while redirect replaces the target without consuming guest cycles. |
| LR-EXEC-5 | Exact translation, execution, and cache telemetry | verified | Runtime tests distinguish successful translations, executed blocks/instructions, and code-LUT hits/misses across cold, cached, stopped, and redirected paths. |
| LR-EXEC-6 | Consumer-controlled typed fallback admission | verified | Clang-built runtime tests prove refusal retains the typed event and exact PC, records a refusal separately, executes zero interpreter instructions, and supersedes an unsafe-fetch native fault; allowed fallback returns to dynarec execution. |
| LR-EXEC-7 | Exact SYSCALL/BREAK exception context | verified | Synthetic shipping-JIT and diagnostic-interpreter tests exercise direct instruction entry, taken/untaken conditional branches, and unconditional jumps. Every exit preserves the trapping instruction PC and cycle count, reports delay-slot context only when executed in that context, and prevents successor execution. JIT cases require nonzero translated execution and zero fallback. |
| LR-CODE-1 | A changed guest range revokes every translated block it overlaps | verified | Eleven runtime cases read the guest back through the execute path: an interior-word write, a two-byte write splitting a four-byte instruction, a write spanning two blocks, a 64-block range, an interior branch-target label, an untranslated range, overlapping and repeated writes, and a whole-index scan proving no code-LUT entry sits outside a block extent. The red-first run failed the interior, partial-word, and spanning cases while the first-word case passed. | 
| LR-CODE-2 | Ranged invalidation stays cheap per guest store | verified | The cost probe measures both paths against a 2049-block cache: a store into an untranslated word and an empty range are single-digit nanoseconds and answered by the translated-word test with no walk, and a store that hits a translated block walks the cache once. The pre-fix tree measured through the same probe. | 
| LR-CODE-3 | Self-modifying detection asks where a store lands, not what its base is | verified | Nineteen synthetic blocks, each with a base register equal to the block's own first word (the shape Spyro 2's `lui $at,0x8006` produces): six stores resolving into the block's own text are still detected and cost at least one interpreter fallback of reason `self_modifying_code`, and thirteen that resolve outside it cost exactly zero fallbacks of any reason and gain native code. The thirteen are exactly the cases the previous base-only comparison refused, so the contract fails on the pre-fix rule by construction. Consumer proof is not claimed here: psxport refuses a dirty `shared/lightrec` (see `docs/upstream.md`), so the fixed runtime has not yet run in a title. |
| LR-HOST-1 | Linux x86_64 native execution | verified | Clang 22.1.8 builds and runs the contract test against system GNU Lightning 2.2.3. |
| LR-HOST-2 | macOS arm64 native execution | blocked | Requires a maintained MAP_JIT/write-protection arena implementation and signed-app test. |
| LR-HOST-3 | Android arm64-v8a native execution | blocked | Requires GNU Lightning x18 reservation plus API-21 device/emulator execution test. |
| LR-HOST-4 | Windows x86_64 native execution | blocked | No maintained Windows GNU Lightning build, executable-memory policy, and synthetic runtime job are pinned and proven yet. |

Current focus: integrate the measured execution contract in consumers. The
maintained GNU Lightning target fixes remain prerequisites before enabling
either AArch64 target.
