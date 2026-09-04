# Codemap

| Responsibility | Owner | Entry points |
|---|---|---|
| Public runtime and metrics ABI | `lightrec.h` | `lightrec_execute`, `lightrec_get_execution_stats`, `lightrec_get_last_fallback` |
| Dispatch, synchronous compilation, typed fallback | `lightrec.c` | `get_next_block_func`, `lightrec_fallback_block` |
| Guest decode and IR | `disassembler.h`, `lightrec.c` | `lightrec_disassemble`, `lightrec_precompile_block` |
| IR optimization | `optimizer.c` | `lightrec_optimize` |
| Native lowering and execution counters | `emitter.c`, `regcache.c` | `lightrec_rec_opcode` |
| Fallback semantics and instruction counters | `interpreter.c` | `lightrec_emulate_block`, `lightrec_record_fallback_instruction` |
| Native block ownership and invalidation | `blockcache.c` | block registration, lookup, invalidation, reclamation |
| Host capability policy | `cmake/LightrecHost.cmake` | `lightrec_require_proven_host` |
| Contract verification | `tests/`, `tools/` | runtime, source-boundary, host-policy, format, analyzer, and structure tests |
