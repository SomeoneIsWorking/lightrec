// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * Code-cache invalidation contract.
 *
 * A block is a run of translated guest words starting at its PC, so a changed
 * range must revoke every block whose extent it overlaps - not only the block
 * that starts inside it. Every case here reads the guest back through the
 * public execute path, so a block that kept running stale code fails even
 * though the invalidation call was made.
 */

#include "blockcache.h"
#include "dynarec_fixture.h"
#include "lightrec-private.h"

#include <inttypes.h>
#include <stdio.h>

/* A syscall one page past the small programs below, used both as a program
 * terminator and as the return address of a program that ends in jr $ra. */
#define SYSCALL_PC 0x100u

#define SWEEP_BLOCKS 64u
#define SWEEP_STRIDE 8u
#define SWEEP_SPAN (SWEEP_BLOCKS * SWEEP_STRIDE * 4u)

static void install_syscall(struct fixture *fixture)
{
	fixture->ram[SYSCALL_PC / sizeof(u32)] = 0x0000000c; /* syscall */
	lightrec_get_registers(fixture->state)->gpr[31] = SYSCALL_PC;
}

static struct lightrec_registers *run_program(struct fixture *fixture, u32 pc)
{
	struct lightrec_registers *registers = lightrec_get_registers(fixture->state);
	u32 saved_ra = registers->gpr[31];

	*registers = (struct lightrec_registers){0};
	registers->gpr[31] = saved_ra;
	lightrec_reset_cycle_count(fixture->state, 0);
	lightrec_execute(fixture->state, pc, 100000);
	return registers;
}

static u64 translated_blocks(const struct fixture *fixture)
{
	return lightrec_get_execution_stats(fixture->state)->translated_blocks;
}

static int expect_register(const struct lightrec_registers *registers, unsigned int index,
			   u32 expected, const char *what)
{
	if (registers->gpr[index] == expected)
		return 0;

	fprintf(stderr, "%s: $%u = 0x%08" PRIx32 ", expected 0x%08" PRIx32 "\n", what, index,
		registers->gpr[index], expected);
	return 1;
}

/* The reported defect: the changed word is a delay slot, not the block's first
 * word. The invalidation is made and returns, and the old code keeps running. */
static int test_interior_word_write_revokes_block(void)
{
	struct lightrec_registers *registers;
	struct fixture fixture;
	int failed = 0;

	if (fixture_init(&fixture, "lightrec_invalidation_contract"))
		return 1;

	fixture.ram[0] = 0x03e00008; /* jr $ra */
	fixture.ram[1] = 0x24020001; /* addiu $v0, $zero, 1 (delay slot) */
	install_syscall(&fixture);

	registers = run_program(&fixture, 0);
	failed |= expect_register(registers, 2, 1, "interior word baseline");

	fixture.ram[1] = 0x24020002; /* addiu $v0, $zero, 2 */
	lightrec_invalidate(fixture.state, 4, 4);

	registers = run_program(&fixture, 0);
	failed |= expect_register(registers, 2, 2, "interior word after write");

	fixture_destroy(&fixture);
	return failed;
}

/* The case that already worked must keep working. */
static int test_first_word_write_revokes_block(void)
{
	struct lightrec_registers *registers;
	struct fixture fixture;
	int failed = 0;

	if (fixture_init(&fixture, "lightrec_invalidation_contract"))
		return 1;

	fixture.ram[0] = 0x24020001; /* addiu $v0, $zero, 1 */
	fixture.ram[1] = 0x03e00008; /* jr $ra */
	fixture.ram[2] = 0x00000000; /* nop (delay slot) */
	install_syscall(&fixture);

	registers = run_program(&fixture, 0);
	failed |= expect_register(registers, 2, 1, "first word baseline");

	fixture.ram[0] = 0x24020007; /* addiu $v0, $zero, 7 */
	lightrec_invalidate(fixture.state, 0, 4);

	registers = run_program(&fixture, 0);
	failed |= expect_register(registers, 2, 7, "first word after write");

	fixture_destroy(&fixture);
	return failed;
}

/* A write that changes no byte must not cost a recompilation: the block keeps
 * its translation, which is the behaviour the first-word path already had. */
static int test_unchanged_word_write_keeps_translation(void)
{
	struct lightrec_registers *registers;
	struct fixture fixture;
	u64 before;
	int failed = 0;

	if (fixture_init(&fixture, "lightrec_invalidation_contract"))
		return 1;

	fixture.ram[0] = 0x03e00008; /* jr $ra */
	fixture.ram[1] = 0x24020001; /* addiu $v0, $zero, 1 (delay slot) */
	install_syscall(&fixture);

	registers = run_program(&fixture, 0);
	failed |= expect_register(registers, 2, 1, "unchanged baseline");
	before = translated_blocks(&fixture);

	lightrec_invalidate(fixture.state, 4, 4);
	registers = run_program(&fixture, 0);
	failed |= expect_register(registers, 2, 1, "unchanged after write");
	if (translated_blocks(&fixture) != before) {
		fprintf(stderr,
			"unchanged write recompiled: translated_blocks %" PRIu64 " -> %" PRIu64
			"\n",
			before, translated_blocks(&fixture));
		failed = 1;
	}

	fixture_destroy(&fixture);
	return failed;
}

/* Two bytes inside the middle of a four-byte instruction, at an address that
 * splits the word it changes. */
static int test_partial_word_write_revokes_block(void)
{
	struct lightrec_registers *registers;
	struct fixture fixture;
	int failed = 0;

	if (fixture_init(&fixture, "lightrec_invalidation_contract"))
		return 1;

	fixture.ram[0] = 0x24020001; /* addiu $v0, $zero, 1 */
	fixture.ram[1] = 0x24030005; /* addiu $v1, $zero, 5 */
	fixture.ram[2] = 0x03e00008; /* jr $ra */
	fixture.ram[3] = 0x00000000; /* nop (delay slot) */
	install_syscall(&fixture);

	registers = run_program(&fixture, 0);
	failed |= expect_register(registers, 3, 5, "partial write baseline");

	/* Bytes 5 and 6 are the top half of the word at 0x04, so the changed
	 * instruction is 0x24030905, and the reported address is not the start
	 * of the word it changes. */
	((u8 *)fixture.ram)[0x05] = 0x09;
	((u8 *)fixture.ram)[0x06] = 0x03;
	lightrec_invalidate(fixture.state, 0x05, 2);

	registers = run_program(&fixture, 0);
	failed |= expect_register(registers, 3, 0x905, "partial write after write");

	fixture_destroy(&fixture);
	return failed;
}

/* One write that starts in the last word of one block and ends inside the
 * next. Both blocks must be revoked, and both changes must be the ones that
 * run. */
static int test_range_spanning_two_blocks_revokes_both(void)
{
	struct lightrec_registers *registers;
	struct fixture fixture;
	int failed = 0;

	if (fixture_init(&fixture, "lightrec_invalidation_contract"))
		return 1;

	fixture.ram[0] = 0x24020001; /* addiu $v0, $zero, 1 */
	fixture.ram[1] = 0x24030003; /* addiu $v1, $zero, 3 */
	fixture.ram[2] = 0x08000005; /* j 0x14 */
	fixture.ram[3] = 0x24040006; /* addiu $v2, $zero, 6 (delay slot) */
	fixture.ram[5] = 0x24020002; /* addiu $v0, $zero, 2 (block two) */
	fixture.ram[6] = 0x24030005; /* addiu $v1, $zero, 5 (block two) */
	fixture.ram[7] = 0x0000000c; /* syscall (block two) */
	install_syscall(&fixture);

	registers = run_program(&fixture, 0);
	failed |= expect_register(registers, 2, 2, "spanning baseline $v0");
	failed |= expect_register(registers, 3, 5, "spanning baseline $v1");
	failed |= expect_register(registers, 4, 6, "spanning baseline $v2");

	/* 0x0c is the last word of the first block, 0x14 its successor's first
	 * word, and 0x18 that block's second word. */
	fixture.ram[3] = 0x24040009; /* addiu $v2, $zero, 9 */
	fixture.ram[5] = 0x24020007; /* addiu $v0, $zero, 7 */
	fixture.ram[6] = 0x24030008; /* addiu $v1, $zero, 8 */
	lightrec_invalidate(fixture.state, 0x0c, 16);

	registers = run_program(&fixture, 0);
	failed |= expect_register(registers, 2, 7, "spanning after write $v0");
	failed |= expect_register(registers, 3, 8, "spanning after write $v1");
	failed |= expect_register(registers, 4, 9, "spanning after write $v2");

	fixture_destroy(&fixture);
	return failed;
}

/* A guest branch can enter the middle of a block through a label the code LUT
 * holds for an interior offset, so revoking a block has to drop that entry as
 * well as the block's first word. */
static int test_interior_branch_target_entry_is_revoked(void)
{
	struct lightrec_registers *registers;
	struct fixture fixture;
	int failed = 0;

	if (fixture_init(&fixture, "lightrec_invalidation_contract"))
		return 1;

	fixture.ram[0] = 0x24080001; /* addiu $t0, $zero, 1 */
	fixture.ram[1] = 0x15000001; /* bne $t0, $zero, 0x0c */
	fixture.ram[2] = 0x00000000; /* nop (delay slot) */
	fixture.ram[3] = 0x24020002; /* addiu $v0, $zero, 2 (interior target) */
	fixture.ram[4] = 0x03e00008; /* jr $ra */
	fixture.ram[5] = 0x00000000; /* nop (delay slot) */
	install_syscall(&fixture);

	registers = run_program(&fixture, 0);
	failed |= expect_register(registers, 2, 2, "interior target baseline");

	fixture.ram[3] = 0x24020007; /* addiu $v0, $zero, 7 */
	lightrec_invalidate(fixture.state, 0x0c, 4);

	registers = run_program(&fixture, 0x0c);
	failed |= expect_register(registers, 2, 7, "interior target after write");

	fixture_destroy(&fixture);
	return failed;
}

/* A write into a range that holds no translated code must leave every
 * translation alone. */
static int test_write_to_untranslated_range_changes_nothing(void)
{
	struct lightrec_registers *registers;
	struct fixture fixture;
	u64 before;
	int failed = 0;

	if (fixture_init(&fixture, "lightrec_invalidation_contract"))
		return 1;

	fixture.ram[0] = 0x03e00008; /* jr $ra */
	fixture.ram[1] = 0x24020001; /* addiu $v0, $zero, 1 (delay slot) */
	install_syscall(&fixture);

	registers = run_program(&fixture, 0);
	failed |= expect_register(registers, 2, 1, "untranslated baseline");
	before = translated_blocks(&fixture);

	/* A heap-shaped address well past every translated block. */
	fixture.ram[0x30000 / sizeof(u32)] = 0xdeadbeef;
	lightrec_invalidate(fixture.state, 0x30000, 0x4000);

	registers = run_program(&fixture, 0);
	failed |= expect_register(registers, 2, 1, "untranslated after write");
	if (translated_blocks(&fixture) != before) {
		fprintf(stderr,
			"untranslated write translated something: translated_blocks "
			"%" PRIu64 " -> %" PRIu64 "\n",
			before, translated_blocks(&fixture));
		failed = 1;
	}

	fixture_destroy(&fixture);
	return failed;
}

/* Overlapping writes, and a repeat of a range that was already revoked: the
 * second write has nothing left to revoke and must not disturb the block that
 * the first one produced. */
static int test_overlapping_and_repeated_writes(void)
{
	struct lightrec_registers *registers;
	struct fixture fixture;
	u64 before;
	int failed = 0;

	if (fixture_init(&fixture, "lightrec_invalidation_contract"))
		return 1;

	fixture.ram[0] = 0x03e00008; /* jr $ra */
	fixture.ram[1] = 0x24020001; /* addiu $v0, $zero, 1 (delay slot) */
	install_syscall(&fixture);

	registers = run_program(&fixture, 0);
	failed |= expect_register(registers, 2, 1, "repeat baseline");

	fixture.ram[1] = 0x24020003; /* addiu $v0, $zero, 3 */
	lightrec_invalidate(fixture.state, 4, 4);
	lightrec_invalidate(fixture.state, 0, 8); /* overlaps the first write */
	lightrec_invalidate(fixture.state, 2, 6); /* and straddles it unaligned */

	registers = run_program(&fixture, 0);
	failed |= expect_register(registers, 2, 3, "repeat after overlapping writes");
	before = translated_blocks(&fixture);

	/* The same bytes again: the block that now holds them must survive. */
	lightrec_invalidate(fixture.state, 4, 4);
	registers = run_program(&fixture, 0);
	failed |= expect_register(registers, 2, 3, "repeat after identical write");
	if (translated_blocks(&fixture) != before) {
		fprintf(stderr,
			"identical repeat write recompiled: translated_blocks %" PRIu64
			" -> %" PRIu64 "\n",
			before, translated_blocks(&fixture));
		failed = 1;
	}

	fixture_destroy(&fixture);
	return failed;
}

/* A range that covers many blocks at once: every block in it must be revoked,
 * which an accumulator proves because a single survivor changes the total. */
static int test_long_range_revokes_every_block(void)
{
	struct lightrec_registers *registers;
	struct fixture fixture;
	u32 expected = 0;
	unsigned int i, w;
	int failed = 0;

	if (fixture_init(&fixture, "lightrec_invalidation_contract"))
		return 1;

	for (i = 0; i < SWEEP_BLOCKS; i++) {
		w = i * SWEEP_STRIDE;
		for (unsigned int j = 0; j < SWEEP_STRIDE - 2; j++)
			fixture.ram[w + j] = 0x24420001; /* addiu $v0, $v0, 1 */
		fixture.ram[w + SWEEP_STRIDE - 2] = 0x08000000 | (w + SWEEP_STRIDE);
		fixture.ram[w + SWEEP_STRIDE - 1] = 0x24420001; /* delay slot */
		expected += SWEEP_STRIDE - 1;
	}
	fixture.ram[SWEEP_BLOCKS * SWEEP_STRIDE] = 0x0000000c; /* syscall */

	registers = run_program(&fixture, 0);
	failed |= expect_register(registers, 2, expected, "sweep baseline");

	for (i = 0; i < SWEEP_BLOCKS; i++) {
		w = i * SWEEP_STRIDE;
		/* Every word except the jump that ends the block. */
		for (unsigned int j = 0; j < SWEEP_STRIDE; j++)
			if (j != SWEEP_STRIDE - 2)
				fixture.ram[w + j] = 0x24420002; /* addiu $v0, $v0, 2 */
	}
	expected *= 2;
	lightrec_invalidate(fixture.state, 0, SWEEP_SPAN);

	registers = run_program(&fixture, 0);
	failed |= expect_register(registers, 2, expected, "sweep after write");

	fixture_destroy(&fixture);
	return failed;
}

/* What a report has to state about an invalidation: how many calls were made,
 * how many code words they examined, whether the translated-word test answered
 * a call alone or a walk ran, how many blocks the walk examined, and how many
 * it revoked. A call that matches nothing is only meaningful beside the counts
 * that show the work actually looked. */
struct invalidation_report {
	u64 invalidations;
	u64 words;
	u64 guards;
	u64 scans;
	u64 invalidated;
};

static struct invalidation_report take_report(const struct fixture *fixture)
{
	const struct lightrec_execution_stats *stats = lightrec_get_execution_stats(fixture->state);
	struct invalidation_report report = {
	    .invalidations = stats->invalidations,
	    .words = stats->invalidation_words,
	    .guards = stats->invalidation_guards,
	    .scans = stats->invalidation_scans,
	    .invalidated = stats->invalidated_blocks,
	};

	return report;
}

static int expect_report(const char *what, struct invalidation_report report, u64 calls, u64 words,
			 u64 guards, u64 scans, u64 invalidated)
{
	if (report.invalidations == calls && report.words == words && report.guards == guards &&
	    report.scans == scans && report.invalidated == invalidated) {
		printf("  %s: calls=%" PRIu64 " words=%" PRIu64 " guards=%" PRIu64 " scans=%" PRIu64
		       " revoked=%" PRIu64 "\n",
		       what, report.invalidations, report.words, report.guards, report.scans,
		       report.invalidated);
		return 0;
	}

	fprintf(stderr,
		"%s: calls=%" PRIu64 " words=%" PRIu64 " guards=%" PRIu64 " scans=%" PRIu64
		" revoked=%" PRIu64 ", expected calls=%" PRIu64 " words=%" PRIu64 " guards=%" PRIu64
		" scans=%" PRIu64 " revoked=%" PRIu64 "\n",
		what, report.invalidations, report.words, report.guards, report.scans,
		report.invalidated, calls, words, guards, scans, invalidated);
	return 1;
}

static void install_two_word_function(struct fixture *fixture)
{
	fixture->ram[0] = 0x03e00008; /* jr $ra */
	fixture->ram[1] = 0x24020001; /* addiu $v0, $zero, 1 (delay slot) */
	install_syscall(fixture);
}

/* A range with nothing translated in it is answered by the translated-word
 * test alone, and the control beside it shows the same call shape walking the
 * cache when there is something to revoke. */
static int test_untranslated_range_reports_what_it_scanned(void)
{
	struct lightrec_registers *registers;
	struct fixture fixture;
	struct invalidation_report report;
	int failed = 0;

	if (fixture_init(&fixture, "lightrec_invalidation_contract"))
		return 1;

	install_two_word_function(&fixture);
	registers = run_program(&fixture, 0);
	failed |= expect_register(registers, 2, 1, "report baseline");

	/* Control: the walk runs and finds the one block this state has. */
	lightrec_invalidate(fixture.state, 0, 4);
	report = take_report(&fixture);
	/* Two blocks exist: the function and the syscall it returns to. The walk
	 * examined both and revoked one. */
	failed |= expect_report("control, one block in range", report, 1, 1, 0, 2, 1);

	/* A 16 KiB range with no translated word in it. */
	lightrec_invalidate(fixture.state, 0x30000, 0x4000);
	report = take_report(&fixture);
	failed |= expect_report("untranslated 16 KiB range", report, 2, 1 + 4096, 1, 2, 1);

	fixture_destroy(&fixture);
	return failed;
}

/* The guard is only sound while every non-empty code-LUT entry lies inside the
 * extent of a registered block: that is what makes "no block covers this range"
 * mean "no stale entry is in this range". This scans the whole compact index
 * and reports how many entries it found and how many no block claims. */
static int test_code_lut_entries_live_inside_a_block_extent(void)
{
	const struct lightrec_execution_stats *stats;
	struct fixture fixture;
	u32 offset, entries = 0, orphans = 0;
	int failed = 0;

	if (fixture_init(&fixture, "lightrec_invalidation_contract"))
		return 1;

	/* A sweep of distinct blocks, so the scan has one entry per block to
	 * place rather than one block swallowing the whole program. */
	for (unsigned int i = 0; i < SWEEP_BLOCKS; i++) {
		unsigned int w = i * SWEEP_STRIDE, j;

		for (j = 0; j < SWEEP_STRIDE - 2; j++)
			fixture.ram[w + j] = 0x24420001;
		fixture.ram[w + SWEEP_STRIDE - 2] = 0x08000000 | (w + SWEEP_STRIDE);
		fixture.ram[w + SWEEP_STRIDE - 1] = 0x24420001;
	}
	fixture.ram[SWEEP_BLOCKS * SWEEP_STRIDE] = 0x0000000c;
	lightrec_execute(fixture.state, 0, 100000);

	stats = lightrec_get_execution_stats(fixture.state);
	if (stats->translated_blocks < SWEEP_BLOCKS) {
		fputs("lut invariant: nothing was translated to scan\n", stderr);
		fixture_destroy(&fixture);
		return 1;
	}

	for (offset = 0; offset < CODE_LUT_SIZE; offset++) {
		u32 guest;

		if (!lut_read(fixture.state, offset))
			continue;
		entries++;

		/* lut_offset() lays the RAM image out first and the BIOS image
		 * after it, so the offset names the guest word directly. */
		guest =
		    offset < (RAM_SIZE >> 2) ? offset << 2 : (offset << 2) - RAM_SIZE + 0xa0000000;
		if (!lightrec_find_block_from_lut(fixture.state->block_cache, offset, guest))
			orphans++;
	}

	printf("  lut invariant: %" PRIu32 " of %u code words held an entry, %" PRIu32
	       " of them outside every block extent\n",
	       entries, (unsigned int)CODE_LUT_SIZE, orphans);
	if (orphans) {
		fprintf(stderr,
			"lut invariant: %" PRIu32
			" non-empty entries are outside any registered block, so the"
			" translated-word guard can skip a stale entry\n",
			orphans);
		failed = 1;
	}

	fixture_destroy(&fixture);
	return failed;
}

struct invalidation_case {
	const char *name;
	int (*run)(void);
};

static const struct invalidation_case cases[] = {
    {"interior word write revokes block", test_interior_word_write_revokes_block},
    {"first word write revokes block", test_first_word_write_revokes_block},
    {"unchanged word write keeps translation", test_unchanged_word_write_keeps_translation},
    {"partial word write revokes block", test_partial_word_write_revokes_block},
    {"range spanning two blocks revokes both", test_range_spanning_two_blocks_revokes_both},
    {"interior branch target entry is revoked", test_interior_branch_target_entry_is_revoked},
    {"write to untranslated range changes nothing",
     test_write_to_untranslated_range_changes_nothing},
    {"overlapping and repeated writes", test_overlapping_and_repeated_writes},
    {"long range revokes every block", test_long_range_revokes_every_block},
    {"untranslated range reports what it scanned", test_untranslated_range_reports_what_it_scanned},
    {"code lut entries live inside a block extent",
     test_code_lut_entries_live_inside_a_block_extent},
};

int main(void)
{
	unsigned int failed = 0, i;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		if (cases[i].run()) {
			printf("FAIL %s\n", cases[i].name);
			failed++;
		} else {
			printf("pass %s\n", cases[i].name);
		}
	}

	printf("invalidation contract: %u of %u cases passed\n",
	       (unsigned int)(sizeof(cases) / sizeof(cases[0])) - failed,
	       (unsigned int)(sizeof(cases) / sizeof(cases[0])));
	return failed ? 1 : 0;
}
