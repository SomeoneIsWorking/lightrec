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

Every attempt records a typed reason and guest PC. Executed fallback opcodes are
counted inside the existing interpreter dispatch, while JIT block entries and
opcodes are counted in emitted code. Explicit diagnostic-only interpretation is
not mixed into fallback metrics.

Consumers must publish both sides of `lightrec_execution_stats`. At minimum,
they must refuse a “dynarec-verified” result unless
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
