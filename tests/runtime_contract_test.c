// SPDX-License-Identifier: LGPL-2.1-or-later

#include "lightrec.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#define RAM_SIZE_BYTES 0x200000u
#define BIOS_SIZE_BYTES 0x80000u
#define SCRATCH_SIZE_BYTES 0x400u

struct fixture {
	struct lightrec_mem_map maps[PSX_MAP_CODE_BUFFER + 1];
	struct lightrec_state *state;
	u32 *ram;
	u8 *bios;
	u8 *scratch;
};

static void fixture_destroy(struct fixture *fixture)
{
	if (fixture->state)
		lightrec_destroy(fixture->state);
	free(fixture->scratch);
	free(fixture->bios);
	free(fixture->ram);
	*fixture = (struct fixture){0};
}

static void cop2_op(struct lightrec_state *state, u32 opcode)
{
	(void)state;
	(void)opcode;
}

static void enable_ram(struct lightrec_state *state, _Bool enable)
{
	(void)state;
	(void)enable;
}

static int fixture_init(struct fixture *fixture)
{
	const struct lightrec_ops ops = {
	    .cop2_op = cop2_op,
	    .enable_ram = enable_ram,
	};

	*fixture = (struct fixture){0};
	fixture->ram = calloc(1, RAM_SIZE_BYTES);
	fixture->bios = calloc(1, BIOS_SIZE_BYTES);
	fixture->scratch = calloc(1, SCRATCH_SIZE_BYTES);
	if (!fixture->ram || !fixture->bios || !fixture->scratch) {
		fixture_destroy(fixture);
		return -1;
	}

	fixture->maps[PSX_MAP_KERNEL_USER_RAM] = (struct lightrec_mem_map){
	    .pc = 0,
	    .length = RAM_SIZE_BYTES,
	    .address = fixture->ram,
	};
	fixture->maps[PSX_MAP_BIOS] = (struct lightrec_mem_map){
	    .pc = 0x1fc00000,
	    .length = BIOS_SIZE_BYTES,
	    .address = fixture->bios,
	};
	fixture->maps[PSX_MAP_SCRATCH_PAD] = (struct lightrec_mem_map){
	    .pc = 0x1f800000,
	    .length = SCRATCH_SIZE_BYTES,
	    .address = fixture->scratch,
	};

	fixture->state = lightrec_init("lightrec_runtime_contract_test", fixture->maps,
				       PSX_MAP_CODE_BUFFER + 1, &ops);
	if (!fixture->state) {
		fixture_destroy(fixture);
		return -1;
	}
	return 0;
}

static _Bool fallback_totals_match(const struct lightrec_execution_stats *stats)
{
	u64 blocks = 0;
	u64 instructions = 0;
	unsigned int reason;

	for (reason = 1; reason < LIGHTREC_FALLBACK_REASON_COUNT; reason++) {
		blocks += stats->fallback_blocks_by_reason[reason];
		instructions += stats->fallback_instructions_by_reason[reason];
	}

	return blocks == stats->fallback_blocks && instructions == stats->fallback_instructions;
}

static void print_stats(const char *label, const struct lightrec_execution_stats *stats)
{
	printf("%s: jit_blocks=%" PRIu64 " jit_instructions=%" PRIu64 " fallback_blocks=%" PRIu64
	       " fallback_instructions=%" PRIu64 "\n",
	       label, stats->jit_blocks, stats->jit_instructions, stats->fallback_blocks,
	       stats->fallback_instructions);
}

static int test_cold_block_jits_without_fallback(void)
{
	struct fixture fixture;
	const struct lightrec_execution_stats *stats;
	struct lightrec_registers *registers;

	if (fixture_init(&fixture))
		return 1;
	fixture.ram[0] = 0x2402002a; /* addiu $v0, $zero, 42 */
	fixture.ram[1] = 0x0000000c; /* syscall */

	lightrec_execute(fixture.state, 0, 100);
	stats = lightrec_get_execution_stats(fixture.state);
	registers = lightrec_get_registers(fixture.state);
	if (registers->gpr[2] != 42 || stats->jit_blocks == 0 || stats->jit_instructions < 2 ||
	    stats->fallback_blocks != 0 || stats->fallback_instructions != 0 ||
	    !lightrec_execution_is_dynarec_dominated(fixture.state)) {
		fputs("cold-block contract failed\n", stderr);
		fixture_destroy(&fixture);
		return 1;
	}
	print_stats("cold-jit", stats);

	fixture_destroy(&fixture);
	return 0;
}

static int test_unsupported_block_has_typed_fallback(void)
{
	struct fixture fixture;
	const struct lightrec_execution_stats *stats;
	const struct lightrec_fallback_event *event;

	if (fixture_init(&fixture))
		return 1;
	fixture.ram[0] = 0x10000001; /* beq with a branch in its delay slot */
	fixture.ram[1] = 0x08000003;
	fixture.ram[2] = 0x00000000;
	fixture.ram[3] = 0x0000000c;

	lightrec_execute(fixture.state, 0, 20);
	stats = lightrec_get_execution_stats(fixture.state);
	event = lightrec_get_last_fallback(fixture.state);
	if (stats->fallback_blocks == 0 || stats->fallback_instructions == 0 ||
	    stats->fallback_blocks_by_reason[LIGHTREC_FALLBACK_UNSUPPORTED_CONTROL_FLOW] == 0 ||
	    event->reason != LIGHTREC_FALLBACK_UNSUPPORTED_CONTROL_FLOW ||
	    !fallback_totals_match(stats) || event->guest_pc != 0) {
		fputs("typed unsupported-control-flow fallback failed\n", stderr);
		fixture_destroy(&fixture);
		return 1;
	}
	print_stats("unsupported-control-flow", stats);

	fixture_destroy(&fixture);
	return 0;
}

static int test_unsafe_fetch_is_not_silent(void)
{
	struct fixture fixture;
	const struct lightrec_execution_stats *stats;
	const struct lightrec_fallback_event *event;
	const u32 invalid_pc = 0x1a000000;

	if (fixture_init(&fixture))
		return 1;
	lightrec_execute(fixture.state, invalid_pc, 20);
	stats = lightrec_get_execution_stats(fixture.state);
	event = lightrec_get_last_fallback(fixture.state);
	if (!(lightrec_exit_flags(fixture.state) & LIGHTREC_EXIT_SEGFAULT) ||
	    stats->fallback_blocks != 1 || stats->fallback_instructions != 0 ||
	    stats->fallback_blocks_by_reason[LIGHTREC_FALLBACK_UNSAFE_FETCH] != 1 ||
	    !fallback_totals_match(stats) || event->reason != LIGHTREC_FALLBACK_UNSAFE_FETCH ||
	    event->guest_pc != invalid_pc) {
		fputs("unsafe fetch was not surfaced and counted\n", stderr);
		fixture_destroy(&fixture);
		return 1;
	}
	print_stats("unsafe-fetch", stats);

	fixture_destroy(&fixture);
	return 0;
}

static int test_diagnostic_interpreter_is_explicit_and_unmixed(void)
{
	struct fixture fixture;
	const struct lightrec_execution_stats *stats;
	struct lightrec_registers *registers;

	if (fixture_init(&fixture))
		return 1;
	fixture.ram[0] = 0x2402002a; /* addiu $v0, $zero, 42 */
	fixture.ram[1] = 0x0000000c; /* syscall */

	lightrec_run_interpreter(fixture.state, 0, 100);
	stats = lightrec_get_execution_stats(fixture.state);
	registers = lightrec_get_registers(fixture.state);
	if (registers->gpr[2] != 42 || stats->jit_blocks != 0 || stats->jit_instructions != 0 ||
	    stats->fallback_blocks != 0 || stats->fallback_instructions != 0 ||
	    lightrec_get_last_fallback(fixture.state)->reason != LIGHTREC_FALLBACK_NONE ||
	    lightrec_execution_is_dynarec_dominated(fixture.state)) {
		fputs("diagnostic interpreter polluted runtime telemetry\n", stderr);
		fixture_destroy(&fixture);
		return 1;
	}
	print_stats("diagnostic-interpreter", stats);

	fixture_destroy(&fixture);
	return 0;
}

int main(void)
{
	if (test_cold_block_jits_without_fallback())
		return 1;
	if (test_unsupported_block_has_typed_fallback())
		return 1;
	if (test_unsafe_fetch_is_not_silent())
		return 1;
	if (test_diagnostic_interpreter_is_explicit_and_unmixed())
		return 1;
	return 0;
}
