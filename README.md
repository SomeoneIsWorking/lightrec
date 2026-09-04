
# Lightrec

This maintained fork is a MIPS-to-host dynamic recompiler for PlayStation
runtimes. It keeps Lightrec's single decode/IR/optimizer/emitter pipeline and
uses [GNU Lightning](https://www.gnu.org/software/lightning/) for native code
emission.

Product execution enters through `lightrec_execute()`. Every cold block is
compiled synchronously before it executes. The interpreter remains linked only
for bounded, typed fallback and the explicit diagnostic-only
`lightrec_run_interpreter()` API. Background “interpret while compiling” and
first-pass interpretation are not product modes.

Consumers can report the real execution mix through
`lightrec_get_execution_stats()` and inspect the most recent fallback through
`lightrec_get_last_fallback()`. `lightrec_execution_is_dynarec_dominated()` is
the minimum conformance guard: a run with no JIT instructions, or at least as
many interpreted instructions as JIT instructions, is not dynarec-dominated.

Linux x86_64 is the currently proven host. macOS arm64 and Android arm64-v8a
are deliberately refused until the GNU Lightning and executable-memory
requirements in [the runtime contract](docs/runtime-contract.md) are satisfied.
Upstream provenance is recorded in [docs/upstream.md](docs/upstream.md).
