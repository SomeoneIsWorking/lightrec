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
| LR-HOST-1 | Linux x86_64 native execution | verified | Clang 22.1.8 builds and runs the contract test against system GNU Lightning 2.2.3. |
| LR-HOST-2 | macOS arm64 native execution | blocked | Requires a maintained MAP_JIT/write-protection arena implementation and signed-app test. |
| LR-HOST-3 | Android arm64-v8a native execution | blocked | Requires GNU Lightning x18 reservation plus API-21 device/emulator execution test. |
| LR-HOST-4 | Windows x86_64 native execution | blocked | No maintained Windows GNU Lightning build, executable-memory policy, and synthetic runtime job are pinned and proven yet. |

Current focus: integrate the measured execution contract in consumers. The
maintained GNU Lightning target fixes remain prerequisites before enabling
either AArch64 target.
