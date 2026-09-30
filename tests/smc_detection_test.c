// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * Self-modifying-store detection contract.
 *
 * The question the optimizer asks about a store is "does its target land in the
 * block's own text?". The target is the base register's known value plus the
 * store's displacement, and a store whose base alone falls inside the block is
 * an ordinary data store. Spyro 2's own printf character emitter is the
 * reported case: its block at 0x8005FFFC runs to 0x80060058, and
 *
 *	80060028  lui  $at,0x8006          ; $at = 0x80060000, inside the block
 *	8006002C  sw   $v0,0x6c58($at)    ; writes 0x80066C58, ordinary data RAM
 *
 * made the whole block uncompilable, because the comparison was against the
 * base alone. The program below reproduces that shape exactly: a base that is
 * the block's own first word, and a store whose displacement says whether the
 * access is code or data.
 *
 * Every case runs a real block through the public execute path, so a false
 * positive is observable as a block that never gains native code, and a missed
 * detection is observable as a block that does.
 */

#include "blockcache.h"
#include "dynarec_fixture.h"
#include "lightrec-private.h"

#include <inttypes.h>
#include <stdio.h>

/* The program lives at 0x10000 so that `lui $at,0x0001` can make the base
 * register equal to the block's own first word: a `lui` result is 64 KiB
 * aligned, and the reported defect is exactly a 64 KiB-aligned base
 * (`lui $at,0x8006` -> 0x80060000) landing inside a block that starts a few
 * words earlier. */
#define PROGRAM_PC 0x10000u
#define PROGRAM_WORDS 4u

/* Where the program leaves, one page on: a syscall. `j` is an unconditional
 * jump, so it and its delay slot end the block, and the syscall ends the run
 * with no register state to arrange and no loop to bound. */
#define SYSCALL_PC 0x10100u

/*	0x10000  lui  $at,0x0001      - the base is the block's own first word
 *	0x10004  <store>              - the case's store, base $at
 *	0x10008  j    0x10100         - ends the block at [0x10000, 0x10010)
 *	0x1000c  nop                   - its delay slot
 */
#define LUI_AT_PROGRAM 0x3c010001u /* lui $at,0x0001 */
#define J_SYSCALL 0x08004040u	   /* j 0x10100 */
#define NOP 0x00000000u
#define SYSCALL 0x0000000cu

/* The assembler register the program loads the base into. */
#define BASE_REG 1u

/* The data register every case stores. The stored value is never read back:
 * what is under test is the address the store resolves to, and that is decided
 * when the block is optimized, before the store executes. */
#define DATA_REG 2u

/* The `base` register alone is inside the block, and this displacement carries
 * the store to 0x11000, one page past the program's end. */
#define DISPLACEMENT_OUTSIDE 0x1000u

/* The I-type encoding of the case's store. A macro rather than a function,
 * because the case table is a static initializer and the addresses under test
 * are the point of the test, not something to compute at run time. */
#define STORE_ENCODING(op, imm)                                                                    \
	(((u32)(op) << 26) | ((u32)BASE_REG << 21) | ((u32)DATA_REG << 16) | (u16)(imm))

static void start_fixture(struct fixture *fixture, const char *name, u32 store)
{
	u32 *program;

	if (fixture_init(fixture, (char *)name)) {
		fprintf(stderr, "%s: could not build the fixture\n", name);
		return;
	}

	program = &fixture->ram[PROGRAM_PC / sizeof(u32)];
	program[0] = LUI_AT_PROGRAM;
	program[1] = store;
	program[2] = J_SYSCALL;
	program[3] = NOP;
	fixture->ram[SYSCALL_PC / sizeof(u32)] = SYSCALL;
}

static int expect_self_modifying(const char *what, u32 store, int want)
{
	struct fixture fixture = {0};
	const struct lightrec_execution_stats *stats;
	struct block *block;
	int failed = 0;
	u64 smc_fallbacks;

	start_fixture(&fixture, what, store);
	if (!fixture.state) {
		fprintf(stderr, "%s: no fixture\n", what);
		return 1;
	}

	lightrec_execute(fixture.state, PROGRAM_PC, 1000);
	stats = lightrec_get_execution_stats(fixture.state);

	block = lightrec_find_block(fixture.state->block_cache, PROGRAM_PC);
	if (!block) {
		fprintf(stderr, "%s: no block was built for pc 0x%x\n", what, PROGRAM_PC);
		fixture_destroy(&fixture);
		return 1;
	}

	if (block->nb_ops != PROGRAM_WORDS) {
		fprintf(stderr, "%s: block covers %u words, expected %u\n", what,
			(unsigned int)block->nb_ops, PROGRAM_WORDS);
		failed = 1;
	}

	/* The opcode list is not evidence: a block whose last op is a jump or a
	 * syscall is marked BLOCK_PRELOAD_PC and its list is released, so reading
	 * per-opcode flags off it after the run reads freed memory. The block flag
	 * and the runtime's own fallback counters are the two observables, and they
	 * have to agree. */
	if (block_has_flag(block, BLOCK_NEVER_COMPILE) != want) {
		fprintf(stderr, "%s: block flags=0x%02x, expected self-modifying=%d\n", what,
			(unsigned int)block->flags, want);
		failed = 1;
	}

	/* An ordinary data store must cost the run exactly zero interpreter
	 * fallbacks, of any reason: that is the whole claim. A store that rewrites
	 * its own block is not bounded by one - it changes the block's own text and
	 * is therefore re-entered - so a self-modifying case only has to fall back
	 * at least once, and the count is reported rather than asserted. */
	smc_fallbacks = stats->fallback_blocks_by_reason[LIGHTREC_FALLBACK_SELF_MODIFYING_CODE];
	if (want ? (smc_fallbacks == 0) : (stats->fallback_blocks != 0 || smc_fallbacks != 0)) {
		fprintf(stderr,
			"%s: self-modifying fallback blocks=%" PRIu64 " of %" PRIu64
			" total fallbacks (wanted %s), translated blocks=%" PRIu64 "\n",
			what, smc_fallbacks, stats->fallback_blocks,
			want ? "at least 1" : "exactly 0", stats->translated_blocks);
		failed = 1;
	}

	/* A block that is not self-modifying must be natively compiled, and one
	 * that is must not be: the flag is the whole reason the interpreter runs
	 * the block, so the translation is the observable consequence. */
	if (!want && !block->function) {
		fprintf(stderr, "%s: a compilable block produced no native code\n", what);
		failed = 1;
	}
	if (want && block->function) {
		fprintf(stderr, "%s: a self-modifying block was translated\n", what);
		failed = 1;
	}

	fixture_destroy(&fixture);
	return failed;
}

struct smc_case {
	const char *name;
	u32 store;
	int want;
};

/* True self-modifying code: a store whose resolved address is one of the
 * block's own words. Each width is asked separately, because a byte, a
 * halfword and a word do not touch the same range. */
static const struct smc_case smc_cases[] = {
    {"sw into own text", STORE_ENCODING(OP_SW, 0x8), 1},
    {"sh into own text", STORE_ENCODING(OP_SH, 0x8), 1},
    {"sb into own text", STORE_ENCODING(OP_SB, 0x8), 1},
    /* The unaligned word stores partly write the word they name: `swl` writes
     * down to its start, `swr` writes up to its end, so an address one byte
     * past the block's last byte still rewrites that last byte. */
    {"swl named one byte past the block", STORE_ENCODING(OP_SWL, 0xd), 1},
    {"swr named in the block's last byte", STORE_ENCODING(OP_SWR, 0xc), 1},
    /* A store that starts at the block's last byte and spans past its end
     * writes the block's text, so its whole width is measured. */
    {"sb spanning the block end", STORE_ENCODING(OP_SB, 0xf), 1},
    /* A word store one byte below the block writes three bytes OF it, so the
     * named address being outside the text does not by itself answer the
     * question. This case is why the rule measures the access and not just the
     * address: it fails if the reach is reduced to one byte everywhere. */
    {"sw one byte below the block", STORE_ENCODING(OP_SW, (u16)-1), 1},
};

/* The reported defect: the base register's value is inside the block and the
 * displacement is not, so the store is ordinary data and the block compiles. */
static const struct smc_case data_cases[] = {
    {"sw to base plus offset", STORE_ENCODING(OP_SW, DISPLACEMENT_OUTSIDE), 0},
    {"sh to base plus offset", STORE_ENCODING(OP_SH, DISPLACEMENT_OUTSIDE), 0},
    {"sb to base plus offset", STORE_ENCODING(OP_SB, DISPLACEMENT_OUTSIDE), 0},
    {"swl to base plus offset", STORE_ENCODING(OP_SWL, DISPLACEMENT_OUTSIDE), 0},
    {"swr to base plus offset", STORE_ENCODING(OP_SWR, DISPLACEMENT_OUTSIDE), 0},
    /* A negative displacement is sign-extended, so a base inside the block
     * with a large negative displacement still names data below it. */
    {"sw below the block", STORE_ENCODING(OP_SW, (u16)-0x1000), 0},
    /* A store that begins exactly where the block ends touches no byte of it,
     * for every width including the two unaligned word stores: their own word
     * is the one after the block's. */
    {"sw starting at the block end", STORE_ENCODING(OP_SW, 0x10), 0},
    {"sh starting at the block end", STORE_ENCODING(OP_SH, 0x10), 0},
    {"sb starting at the block end", STORE_ENCODING(OP_SB, 0x10), 0},
    {"swl starting at the block end", STORE_ENCODING(OP_SWL, 0x10), 0},
    {"swr starting at the block end", STORE_ENCODING(OP_SWR, 0x10), 0},
    /* A store that ends exactly where the block begins touches no byte of it. */
    {"sh ending at the block start", STORE_ENCODING(OP_SH, (u16)-0x10), 0},
    {"sw ending at the block start", STORE_ENCODING(OP_SW, (u16)-0x10), 0},
    /* One byte below the block, where the pair above shows a `sw` does reach
     * in: the unaligned word stores do not. Each writes only within the word
     * ending at the byte they name, which is entirely below the block. These
     * two fail if the unaligned stores are given a four-byte reach, which is
     * the same false positive the original base-only comparison had. */
    {"swl one byte below the block", STORE_ENCODING(OP_SWL, (u16)-1), 0},
    {"swr one byte below the block", STORE_ENCODING(OP_SWR, (u16)-1), 0},
};

static int run_cases(const char *group, const struct smc_case *cases, unsigned int count)
{
	unsigned int i;
	int failed = 0;

	for (i = 0; i < count; i++)
		failed += expect_self_modifying(cases[i].name, cases[i].store, cases[i].want);

	printf("%s: %u of %u cases passed\n", group, count - failed, count);
	return failed;
}

int main(void)
{
	int failed = 0;

	failed += run_cases("self-modifying stores detected", smc_cases,
			    sizeof(smc_cases) / sizeof(smc_cases[0]));
	failed += run_cases("ordinary data stores not flagged", data_cases,
			    sizeof(data_cases) / sizeof(data_cases[0]));

	printf("self-modifying detection contract: %s\n", failed ? "FAILED" : "all cases passed");
	return failed ? 1 : 0;
}
