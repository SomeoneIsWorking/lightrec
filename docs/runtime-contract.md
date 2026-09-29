# Runtime contract

## Execution

`lightrec_execute()` synchronously compiles a cold or invalidated block before
executing it. A block may enter the interpreter only through a named fallback:

- self-modifying code that cannot safely continue in the translated block;
- unsupported control flow, including a branch in a branch delay slot;
- native-code compilation failure;
- a load-delay hazard that needs the existing semantic helper;
- an unsafe guest fetch, which is recorded with zero interpreted instructions
  and terminates with `LIGHTREC_EXIT_SEGFAULT` because no safe opcode exists to
  execute.

Every attempt records a typed reason, guest PC, and host error in the latest
fallback event. Executed fallback opcodes are counted inside the existing
interpreter dispatch, while JIT block entries and opcodes are counted in
emitted code. Explicit diagnostic-only interpretation is not mixed into
fallback metrics.

An optional `lightrec_ops.fallback_admission` callback runs at the single
fallback boundary, before fallback counters advance or any interpreter helper
runs. It receives a borrowed `lightrec_fallback_event` and returns
`LIGHTREC_FALLBACK_ALLOW` or `LIGHTREC_FALLBACK_REFUSE`; any value other than
ALLOW fails closed. Refusal leaves the latest event available through
`lightrec_get_last_fallback()`, increments `refused_fallback_blocks` and its
per-reason counter, preserves the exact refused PC, and exits with
`LIGHTREC_EXIT_FALLBACK_REFUSED`. It does not increment admitted fallback block
or instruction counters. An allowed interpreter fallback advances the existing
fallback counters and returns to the dispatcher, so subsequent blocks resume
dynarec execution. Zero-instruction unsafe-fetch and allocation-failure events
also pass through admission; when admitted they retain their native
`LIGHTREC_EXIT_SEGFAULT` or `LIGHTREC_EXIT_NOMEM` result.

An optional `lightrec_ops.block_boundary` callback runs before every guest
basic block reached by `lightrec_execute()`. It observes the exact next guest
PC before cache lookup or execution. Returning `LIGHTREC_BLOCK_STOP` exits with
`LIGHTREC_EXIT_BLOCK_BOUNDARY` and leaves that PC unexecuted. Returning
`LIGHTREC_BLOCK_REDIRECT` replaces the block with `redirect_pc`; the redirected
PC passes through the callback again before lookup. Hook-enabled states keep
intra-translation control-flow targets as dispatcher boundaries, so cached and
directly linked blocks cannot bypass the callback. The diagnostic-only
`lightrec_run_interpreter()` entry point does not invoke this runtime hook.

`lightrec_set_store_observer()` is a diagnostic-only, per-state selected-PC
boundary for ordinary translated `SW` instructions. It accepts up to eight
aligned guest PCs, including an unreachable control, and may be configured only
between `lightrec_execute()` calls. Arming or disarming retires cached guest
translations so an unchanged warm block cannot silently omit or retain the
hook. When a selected store executes, the callback receives exact original
guest PC, a before/after phase, coherent RAM, flushed architectural registers,
and guest cycle. Disarming emits no observer code in subsequent translations.
The callback must only read state; it must not re-enter Lightrec or modify guest
registers or memory.

The hook refuses an observed block with `LIGHTREC_EXIT_OBSERVER_UNSUPPORTED`
before executing any of it if the selected source PC is not a plain `SW`, is a
delay slot, is interpreted/specialized, or its opcode or source PC moved during
optimization. This refusal is separate from interpreter fallback. An
unreached target yields zero callbacks; consumers pair that count with
`executed_instructions` and fallback totals to distinguish a scanned negative
from a route that never ran. The limited store contract is deliberate: it does
not claim exact-PC coverage for other opcodes or transformed store sequences.

`translated_blocks` and `translated_instructions` count successful translation
events, including retranslations. `executed_blocks` and
`executed_instructions` count emitted guest work that actually ran; the old
`jit_blocks` and `jit_instructions` names are compatibility aliases.
`cache_hits` counts block dispatches resolved directly by the code LUT, while
`cache_misses` counts dispatches requiring synchronous lookup, compilation, or
bounded fallback.

## Changed guest code

`lightrec_invalidate(state, addr, len)` reports that a guest RAM range changed,
whether the write came from an emulated store, a DMA, a module load, a
savestate, or the host's own memory API. A block is a run of translated words
starting at its PC, so the contract is that **every registered block whose
extent overlaps the range is revoked**, not only a block that starts inside it.
An interior-word write is therefore honoured, and so is a two-byte write that
splits a four-byte instruction.

Revocation drops the block's whole code-LUT span, which covers both its first
word and any interior label a guest branch can enter it through. The block keeps
its memory and revalidates on the next entry: `lightrec_block_is_outdated()`
compares the guest bytes against the hash taken at translation, restores the LUT
entry when they are unchanged, and recompiles the block when they are not. A
write that changes no byte therefore costs a hash comparison and not a
recompilation, and nothing is freed during invalidation, which is what makes the
call safe from inside a running block - the case a self-modifying store reaches.
`lightrec_invalidate_all()` empties the code LUT instead of walking, which
revokes every block at once and is correct for the same reason: every block then
revalidates on its next entry.

The call runs for every guest store, so it is guarded. The block cache records
which code words a block was registered over, and a range holding none of them
is answered by that test alone. The record is per code word and is never
cleared, so a word that only ever held translated code can cost one wasted walk
later, while a word that never held code is never walked for. Reclamation and
range invalidation share one walk of the block cache.

`invalidations` counts the calls, `invalidation_words` the code words they
examined, `invalidation_guards` the calls the translated-word test answered on
its own, `invalidation_scans` the registered blocks a walk examined, and
`invalidated_blocks` the blocks a walk revoked. A consumer that reports no
invalidation must publish those denominators with it: a zero with
`invalidation_guards` above zero is a scanned negative, and a zero with
`invalidation_scans` above zero is a walk that looked and matched nothing.

The two paths cost very differently, and `tests/invalidation_cost_probe.c`
measures both. A store into a word no block covers, and an empty range, are
answered by the translated-word test alone in single-digit nanoseconds, which is
the case that runs for every guest store. A store that does hit a translated
block walks the cache, which costs microseconds; that is a guest store landing
inside translated code - self-modifying code, a module load, a decompression -
and not the per-frame data-store path. The record is deliberately never
cleared, so a repeated write of an already-revoked range still pays the walk;
clearing it would make the guard unsound if a revalidation restored the block
without re-marking its words, which is the same class of defect this contract
exists to prevent.

Consumers must publish JIT, admitted fallback, and refused fallback totals from
`lightrec_execution_stats`. At minimum, they must refuse a “dynarec-verified” result unless
`lightrec_execution_is_dynarec_dominated()` returns true. Stricter project
thresholds are expected for performance qualification.

## Host capability

Linux x86_64 is covered by the executable runtime contract test. The build
refuses every unproven host rather than producing an ABI-unsafe JIT.

GNU Lightning 2.2.3 advertises AArch64, but that alone is insufficient. The
audited upstream is the [official 2.2.3 release](https://ftp.gnu.org/gnu/lightning/lightning-2.2.3.tar.gz),
and alternate-buffer protection semantics are documented in its
[Customizations manual](https://www.gnu.org/software/lightning/manual/html_node/Customizations.html):

- On macOS arm64, Lightning's owned mapping lacks `MAP_JIT`, and its alternate
  code-buffer protection calls are no-ops. A maintained implementation must own
  one `MAP_JIT` arena and balance Apple per-thread JIT write protection around
  every arena mutation. That includes TLSF initialization, allocation, resize,
  free, emission, and teardown because TLSF metadata lives inside the arena.
  The application also needs the JIT entitlement and instruction-cache
  invalidation.
- On Android arm64-v8a, GNU Lightning 2.2.3 exposes platform register `x18` to
  its allocator. A maintained GNU Lightning fork must reserve `x18` for
  `__ANDROID__`; compiler flags cannot constrain hand-emitted code. The result
  still needs an API-21 device/emulator execution and cache-invalidation test.

The platform requirements come from Apple's
[Apple-silicon JIT guidance](https://developer.apple.com/documentation/apple-silicon/porting-just-in-time-compilers-to-apple-silicon)
and [JIT entitlement reference](https://developer.apple.com/documentation/bundleresources/entitlements/com.apple.security.cs.allow-jit),
plus Android's [arm64-v8a ABI contract](https://developer.android.com/ndk/guides/abis).

The minimum target discriminator emits a function returning value A, replaces
it in the same managed arena with a function returning B, and observes B. The
macOS test additionally verifies balanced write windows and the Android test
holds an x18 sentinel across register-pressure and call-heavy generated code.
