# Codemap

| Responsibility | Owner | Entry points |
|---|---|---|
| Public runtime, callback, and metrics ABI | `lightrec.h`, `execution.c` | `lightrec_execute`, `lightrec_ops.block_boundary`, `lightrec_ops.fallback_admission`, `lightrec_get_execution_stats`, `lightrec_get_last_fallback` |
| Dispatch, block-boundary actions, synchronous compilation, typed fallback admission | `lightrec.c`, `execution.c` | `lightrec_run_block_boundary`, `get_next_block_func`, `lightrec_begin_fallback`, `lightrec_fallback_block` |
| Guest decode and IR | `disassembler.h`, `lightrec.c` | `lightrec_disassemble`, `lightrec_precompile_block` |
| IR optimization | `optimizer.c` | `lightrec_optimize` |
| Native lowering and execution counters | `emitter.c`, `regcache.c` | `lightrec_rec_opcode` |
| Fallback semantics and instruction counters | `interpreter.c` | `lightrec_emulate_block`, `lightrec_record_fallback_instruction` |
| Native block ownership and invalidation | `blockcache.c` | block registration, lookup, invalidation, reclamation |
| Host capability policy | `cmake/LightrecHost.cmake` | `lightrec_require_proven_host` |
| Contract verification | `tests/`, `tools/` | runtime, source-boundary, host-policy, format, analyzer, and structure tests |
