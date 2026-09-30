# Codemap

| Responsibility | Owner | Entry points |
|---|---|---|
| Public runtime, callback, and metrics ABI | `lightrec.h`, `execution.c` | `lightrec_execute`, `lightrec_ops.block_boundary`, `lightrec_ops.fallback_admission`, `lightrec_get_execution_stats`, `lightrec_get_last_fallback` |
| Dispatch, block-boundary actions, synchronous compilation, typed fallback admission | `lightrec.c`, `execution.c` | `lightrec_run_block_boundary`, `get_next_block_func`, `lightrec_begin_fallback`, `lightrec_fallback_block` |
| Guest decode and IR | `disassembler.h`, `lightrec.c` | `lightrec_disassemble`, `lightrec_precompile_block` |
| IR optimization | `optimizer.c`, `optimizer-delay-slots.c` | `lightrec_optimize`, `lightrec_can_switch_delay_slot` (register and exception-order scheduling constraints) |
| Native lowering and execution counters | `emitter.c`, `execution.c`, `regcache.c` | `lightrec_rec_opcode` |
| Selected translated-store observer | `lightrec.h`, `lightrec.c`, `store-observer.c`, `emitter.c` | `lightrec_set_store_observer`; source-PC provenance in `disassembler.h`/`optimizer-delay-slots.c`, cache retirement in `blockcache.c` |
| Fallback semantics and instruction counters | `interpreter.c` | `lightrec_emulate_block`, `lightrec_record_fallback_instruction` |
| Trapping instruction PC and delay-slot context | `emitter.c`, `interpreter.c`, `lightrec-private.h` | `lightrec_exception_flags`; `LIGHTREC_EXIT_EXCEPTION_DELAY_SLOT` qualifies public SYSCALL/BREAK exits |
| Native block ownership and invalidation | `blockcache.c` | block registration, lookup, the one cache walk shared by reclamation and `lightrec_invalidate_blocks`; block extent and the one guest address rule shared by invalidation and self-modifying detection in `block_end_pc`/`block_overlaps_range`/`block_effective_address`/`block_writes_own_text` (`lightrec-private.h`) |
| Host capability policy | `cmake/LightrecHost.cmake` | `lightrec_require_proven_host` |
| Self-modifying-store detection | `optimizer.c` (`lightrec_flag_io`) | marks a block `BLOCK_NEVER_COMPILE` only when a store's resolved effective address overlaps the block's own translated text, for every store width including the unaligned `swl`/`swr`; the address rule itself is `block_effective_address`/`block_writes_own_text` (`lightrec-private.h`) |
| Contract verification | `tests/`, `tools/` | runtime, changed-code invalidation, and self-modifying-store detection contracts, the invalidation cost probe, the shared `tests/dynarec_fixture.h` machine, source-boundary, host-policy, format, analyzer, and structure tests |
