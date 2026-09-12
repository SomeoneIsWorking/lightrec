// SPDX-License-Identifier: LGPL-2.1-or-later

#include "lightrec.h"

#include <errno.h>
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

enum boundary_test_mode {
	BOUNDARY_CONTINUE,
	BOUNDARY_STOP_AT_PC,
	BOUNDARY_REDIRECT_AT_PC,
};

struct boundary_test_context {
	enum boundary_test_mode mode;
	u32 match_pc;
	u32 redirect_pc;
	u32 observed_pcs[8];
	u32 observed_cycles[8];
	unsigned int call_count;
};

enum fallback_test_mode {
	FALLBACK_TEST_ALLOW,
	FALLBACK_TEST_REFUSE,
};

struct fallback_test_context {
	enum fallback_test_mode mode;
	struct lightrec_fallback_event observed_event;
	u64 observed_jit_blocks;
	u64 observed_jit_instructions;
	unsigned int call_count;
};

struct store_observer_context {
	u32 *ram;
	u32 pcs[4];
	u32 values[4];
	u32 source_regs[4];
	u32 cycles[4];
	enum lightrec_store_observer_phase phases[4];
	unsigned int calls;
	unsigned int sentinel_hits;
};

static void observe_store(const struct lightrec_registers *registers, u32 guest_pc,
			  enum lightrec_store_observer_phase phase, u32 cycle, void *user_data)
{
	struct store_observer_context *context = user_data;
	unsigned int index = context->calls++;

	if (guest_pc == 0xfffffffc)
		context->sentinel_hits++;
	if (index >= 4)
		return;
	context->pcs[index] = guest_pc;
	context->values[index] = context->ram[0x40 / sizeof(u32)];
	context->source_regs[index] = registers->gpr[9];
	context->cycles[index] = cycle;
	context->phases[index] = phase;
}

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

static enum lightrec_block_boundary_action
block_boundary(struct lightrec_state *state, u32 guest_pc, u32 *redirect_pc, void *user_data)
{
	struct boundary_test_context *context = user_data;

	if (context->call_count <
	    sizeof(context->observed_pcs) / sizeof(context->observed_pcs[0])) {
		context->observed_pcs[context->call_count] = guest_pc;
		context->observed_cycles[context->call_count] = lightrec_current_cycle_count(state);
	}
	context->call_count++;

	if (context->mode == BOUNDARY_STOP_AT_PC && guest_pc == context->match_pc)
		return LIGHTREC_BLOCK_STOP;
	if (context->mode == BOUNDARY_REDIRECT_AT_PC && guest_pc == context->match_pc) {
		*redirect_pc = context->redirect_pc;
		return LIGHTREC_BLOCK_REDIRECT;
	}
	return LIGHTREC_BLOCK_CONTINUE;
}

static enum lightrec_fallback_action fallback_admission(struct lightrec_state *state,
							const struct lightrec_fallback_event *event,
							void *user_data)
{
	struct fallback_test_context *context = user_data;
	const struct lightrec_execution_stats *stats = lightrec_get_execution_stats(state);

	context->observed_event = *event;
	context->observed_jit_blocks = stats->executed_blocks;
	context->observed_jit_instructions = stats->executed_instructions;
	context->call_count++;
	return context->mode == FALLBACK_TEST_ALLOW ? LIGHTREC_FALLBACK_ALLOW
						    : LIGHTREC_FALLBACK_REFUSE;
}

static int fixture_init_with_callbacks(struct fixture *fixture,
				       struct boundary_test_context *boundary_context,
				       struct fallback_test_context *fallback_context)
{
	const struct lightrec_ops ops = {
	    .cop2_op = cop2_op,
	    .enable_ram = enable_ram,
	    .block_boundary = boundary_context ? block_boundary : NULL,
	    .block_boundary_data = boundary_context,
	    .fallback_admission = fallback_context ? fallback_admission : NULL,
	    .fallback_admission_data = fallback_context,
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

static int fixture_init_with_boundary(struct fixture *fixture,
				      struct boundary_test_context *boundary_context)
{
	return fixture_init_with_callbacks(fixture, boundary_context, NULL);
}

static int fixture_init(struct fixture *fixture)
{
	return fixture_init_with_callbacks(fixture, NULL, NULL);
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

static _Bool refused_fallback_totals_match(const struct lightrec_execution_stats *stats)
{
	u64 blocks = 0;
	unsigned int reason;

	for (reason = 1; reason < LIGHTREC_FALLBACK_REASON_COUNT; reason++)
		blocks += stats->refused_fallback_blocks_by_reason[reason];

	return blocks == stats->refused_fallback_blocks;
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

static int test_fallback_refusal_executes_no_interpreter_instructions(void)
{
	struct fallback_test_context fallback = {.mode = FALLBACK_TEST_REFUSE};
	const struct lightrec_execution_stats *stats;
	const struct lightrec_fallback_event *event;
	struct fixture fixture;
	u32 next_pc;

	if (fixture_init_with_callbacks(&fixture, NULL, &fallback))
		return 1;
	fixture.ram[0] = 0x10000001; /* beq with a branch in its delay slot */
	fixture.ram[1] = 0x08000003;
	fixture.ram[2] = 0x2402002a; /* must remain unexecuted by the interpreter */
	fixture.ram[3] = 0x0000000c;

	next_pc = lightrec_execute(fixture.state, 0, 20);
	stats = lightrec_get_execution_stats(fixture.state);
	event = lightrec_get_last_fallback(fixture.state);
	if (next_pc != 0 || fallback.call_count != 1 ||
	    fallback.observed_event.reason != LIGHTREC_FALLBACK_UNSUPPORTED_CONTROL_FLOW ||
	    fallback.observed_event.guest_pc != 0 || fallback.observed_event.host_error != 0 ||
	    event->reason != LIGHTREC_FALLBACK_UNSUPPORTED_CONTROL_FLOW || event->guest_pc != 0 ||
	    !(lightrec_exit_flags(fixture.state) & LIGHTREC_EXIT_FALLBACK_REFUSED) ||
	    stats->fallback_blocks != 0 || stats->fallback_instructions != 0 ||
	    stats->refused_fallback_blocks != 1 ||
	    stats->refused_fallback_blocks_by_reason[LIGHTREC_FALLBACK_UNSUPPORTED_CONTROL_FLOW] !=
		1 ||
	    !fallback_totals_match(stats) || !refused_fallback_totals_match(stats) ||
	    lightrec_get_registers(fixture.state)->gpr[2] != 0) {
		fputs("fallback refusal executed or lost its typed event\n", stderr);
		fixture_destroy(&fixture);
		return 1;
	}

	fixture_destroy(&fixture);
	return 0;
}

static int test_allowed_fallback_resumes_dynarec(void)
{
	struct fallback_test_context fallback = {.mode = FALLBACK_TEST_ALLOW};
	const struct lightrec_execution_stats *stats;
	struct fixture fixture;

	if (fixture_init_with_callbacks(&fixture, NULL, &fallback))
		return 1;
	fixture.ram[0] = 0x10000001; /* beq with a branch in its delay slot */
	fixture.ram[1] = 0x08000003;
	fixture.ram[2] = 0x00000000;
	fixture.ram[3] = 0x2402002a; /* dynarec resumes here */
	fixture.ram[4] = 0x0000000c;

	lightrec_execute(fixture.state, 0, 40);
	stats = lightrec_get_execution_stats(fixture.state);
	if (fallback.call_count != 1 ||
	    fallback.observed_event.reason != LIGHTREC_FALLBACK_UNSUPPORTED_CONTROL_FLOW ||
	    stats->fallback_blocks != 1 || stats->fallback_instructions == 0 ||
	    stats->refused_fallback_blocks != 0 ||
	    stats->executed_blocks <= fallback.observed_jit_blocks ||
	    stats->executed_instructions <= fallback.observed_jit_instructions ||
	    lightrec_get_registers(fixture.state)->gpr[2] != 42 || !fallback_totals_match(stats) ||
	    !refused_fallback_totals_match(stats)) {
		fputs("admitted fallback did not resume dynarec execution\n", stderr);
		fixture_destroy(&fixture);
		return 1;
	}

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

static int test_unsafe_fetch_refusal_precedes_native_fault(void)
{
	struct fallback_test_context fallback = {.mode = FALLBACK_TEST_REFUSE};
	const struct lightrec_execution_stats *stats;
	const struct lightrec_fallback_event *event;
	const u32 invalid_pc = 0x1a000000;
	struct fixture fixture;

	if (fixture_init_with_callbacks(&fixture, NULL, &fallback))
		return 1;
	if (lightrec_execute(fixture.state, invalid_pc, 20) != invalid_pc) {
		fputs("unsafe-fetch refusal lost the exact guest PC\n", stderr);
		fixture_destroy(&fixture);
		return 1;
	}

	stats = lightrec_get_execution_stats(fixture.state);
	event = lightrec_get_last_fallback(fixture.state);
	if (fallback.call_count != 1 ||
	    fallback.observed_event.reason != LIGHTREC_FALLBACK_UNSAFE_FETCH ||
	    fallback.observed_event.guest_pc != invalid_pc ||
	    fallback.observed_event.host_error != -EFAULT ||
	    event->reason != LIGHTREC_FALLBACK_UNSAFE_FETCH || event->guest_pc != invalid_pc ||
	    event->host_error != -EFAULT ||
	    lightrec_exit_flags(fixture.state) != LIGHTREC_EXIT_FALLBACK_REFUSED ||
	    stats->fallback_blocks != 0 || stats->fallback_instructions != 0 ||
	    stats->refused_fallback_blocks != 1 ||
	    stats->refused_fallback_blocks_by_reason[LIGHTREC_FALLBACK_UNSAFE_FETCH] != 1 ||
	    !fallback_totals_match(stats) || !refused_fallback_totals_match(stats)) {
		fputs("unsafe-fetch fallback bypassed admission\n", stderr);
		fixture_destroy(&fixture);
		return 1;
	}

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

static int test_boundary_callback_observes_cache_hit(void)
{
	struct boundary_test_context boundary = {.mode = BOUNDARY_CONTINUE};
	const struct lightrec_execution_stats *stats;
	struct fixture fixture;
	u64 first_translations;
	u32 target_cycle;

	if (fixture_init_with_boundary(&fixture, &boundary))
		return 1;
	fixture.ram[0] = 0x2402002a; /* addiu $v0, $zero, 42 */
	fixture.ram[1] = 0x0000000c; /* syscall */

	lightrec_execute(fixture.state, 0, 100);
	stats = lightrec_get_execution_stats(fixture.state);
	first_translations = stats->translated_blocks;
	target_cycle = lightrec_current_cycle_count(fixture.state) + 100;
	lightrec_execute(fixture.state, 0, target_cycle);
	stats = lightrec_get_execution_stats(fixture.state);

	if (boundary.call_count != 2 || boundary.observed_pcs[0] != 0 ||
	    boundary.observed_pcs[1] != 0 || first_translations != 1 ||
	    stats->translated_blocks != 1 || stats->cache_misses != 1 || stats->cache_hits != 1 ||
	    stats->executed_blocks != 2 || stats->executed_instructions != 4) {
		fputs("block-boundary cache-hit contract failed\n", stderr);
		fixture_destroy(&fixture);
		return 1;
	}

	fixture_destroy(&fixture);
	return 0;
}

static int test_selected_interior_store_observer(void)
{
	const u32 targets[] = {8, 0xfffffffc};
	const u32 unsupported_target = 4;
	struct store_observer_context observer = {0};
	struct fixture fixture;
	struct lightrec_registers *registers;
	const struct lightrec_execution_stats *stats;
	u64 warm_translations;
	u64 before_unsupported_instructions;
	u32 plain_cycles, plain_result;
	u32 unsupported_pc;

	if (fixture_init(&fixture))
		return 1;
	fixture.ram[0] = 0x24080040; /* addiu $t0, $zero, 0x40 */
	fixture.ram[1] = 0x24090007; /* addiu $t1, $zero, 7 */
	fixture.ram[2] = 0xad090000; /* sw $t1, 0($t0): interior store */
	fixture.ram[3] = 0x240a0009; /* addiu $t2, $zero, 9 */
	fixture.ram[4] = 0x0000000c; /* syscall */
	observer.ram = fixture.ram;

	lightrec_execute(fixture.state, 0, 100);
	stats = lightrec_get_execution_stats(fixture.state);
	registers = lightrec_get_registers(fixture.state);
	warm_translations = stats->translated_blocks;
	plain_cycles = lightrec_current_cycle_count(fixture.state);
	plain_result = registers->gpr[10];
	if (warm_translations == 0 || stats->fallback_blocks != 0 ||
	    stats->fallback_instructions != 0 || fixture.ram[0x40 / sizeof(u32)] != 7 ||
	    plain_result != 9) {
		fputs("unobserved interior-store JIT baseline failed\n", stderr);
		fixture_destroy(&fixture);
		return 1;
	}

	fixture.ram[0x40 / sizeof(u32)] = 0;
	*registers = (struct lightrec_registers){0};
	lightrec_reset_cycle_count(fixture.state, 0);
	if (lightrec_set_store_observer(fixture.state, targets, 2, observe_store, &observer)) {
		fputs("failed to arm selected-store observer\n", stderr);
		fixture_destroy(&fixture);
		return 1;
	}

	lightrec_execute(fixture.state, 0, 100);
	stats = lightrec_get_execution_stats(fixture.state);
	if (observer.calls != 2 || observer.sentinel_hits != 0 || observer.pcs[0] != 8 ||
	    observer.pcs[1] != 8 || observer.phases[0] != LIGHTREC_STORE_BEFORE ||
	    observer.phases[1] != LIGHTREC_STORE_AFTER || observer.values[0] != 0 ||
	    observer.values[1] != 7 || observer.source_regs[0] != 7 ||
	    observer.source_regs[1] != 7 || observer.cycles[1] <= observer.cycles[0] ||
	    lightrec_current_cycle_count(fixture.state) != plain_cycles ||
	    registers->gpr[10] != plain_result || fixture.ram[0x40 / sizeof(u32)] != 7 ||
	    stats->translated_blocks <= warm_translations || stats->executed_instructions < 10 ||
	    stats->fallback_blocks != 0 || stats->fallback_instructions != 0) {
		fputs("selected interior-store pre/post or sentinel contract failed\n", stderr);
		fixture_destroy(&fixture);
		return 1;
	}
	printf("selected store: hits=2, unreachable=0/%" PRIu64 " JIT instructions, fallback=0\n",
	       stats->executed_instructions);

	if (lightrec_set_store_observer(fixture.state, NULL, 0, NULL, NULL)) {
		fputs("failed to disarm selected-store observer\n", stderr);
		fixture_destroy(&fixture);
		return 1;
	}
	fixture.ram[0x40 / sizeof(u32)] = 0;
	*registers = (struct lightrec_registers){0};
	lightrec_reset_cycle_count(fixture.state, 0);
	lightrec_execute(fixture.state, 0, 100);
	stats = lightrec_get_execution_stats(fixture.state);
	if (observer.calls != 2 || lightrec_current_cycle_count(fixture.state) != plain_cycles ||
	    registers->gpr[10] != plain_result || fixture.ram[0x40 / sizeof(u32)] != 7 ||
	    stats->fallback_blocks != 0 || stats->fallback_instructions != 0) {
		fputs("disarmed selected-store observer changed JIT behavior\n", stderr);
		fixture_destroy(&fixture);
		return 1;
	}

	fixture.ram[0x40 / sizeof(u32)] = 0;
	*registers = (struct lightrec_registers){0};
	lightrec_reset_cycle_count(fixture.state, 0);
	before_unsupported_instructions = stats->executed_instructions;
	if (lightrec_set_store_observer(fixture.state, &unsupported_target, 1, observe_store,
					&observer)) {
		fputs("failed to arm unsupported target control\n", stderr);
		fixture_destroy(&fixture);
		return 1;
	}
	unsupported_pc = lightrec_execute(fixture.state, 0, 100);
	stats = lightrec_get_execution_stats(fixture.state);
	if (unsupported_pc != 0 ||
	    !(lightrec_exit_flags(fixture.state) & LIGHTREC_EXIT_OBSERVER_UNSUPPORTED) ||
	    observer.calls != 2 || fixture.ram[0x40 / sizeof(u32)] != 0 ||
	    stats->executed_instructions != before_unsupported_instructions ||
	    stats->fallback_blocks != 0 || stats->fallback_instructions != 0) {
		fputs("unsupported selected-PC target did not fail closed\n", stderr);
		fixture_destroy(&fixture);
		return 1;
	}

	fixture_destroy(&fixture);
	return 0;
}

static int test_reordered_store_observer_refuses_false_pc(void)
{
	struct store_observer_context observer = {0};
	const struct lightrec_execution_stats *stats;
	struct fixture fixture;
	u32 target = 8;
	u32 next_pc;

	if (fixture_init(&fixture))
		return 1;
	fixture.ram[0] = 0x24080040; /* addiu $t0, $zero, 0x40 */
	fixture.ram[1] = 0x8d090000; /* lw $t1, 0($t0) */
	fixture.ram[2] = 0xad090004; /* sw $t1, 4($t0), moved for load delay */
	fixture.ram[3] = 0x0000000c; /* syscall */
	fixture.ram[0x40 / sizeof(u32)] = 9;
	observer.ram = fixture.ram;
	if (lightrec_set_store_observer(fixture.state, &target, 1, observe_store, &observer)) {
		fputs("failed to arm reordered-store control\n", stderr);
		fixture_destroy(&fixture);
		return 1;
	}
	next_pc = lightrec_execute(fixture.state, 0, 100);
	stats = lightrec_get_execution_stats(fixture.state);
	if (next_pc != 0 ||
	    !(lightrec_exit_flags(fixture.state) & LIGHTREC_EXIT_OBSERVER_UNSUPPORTED) ||
	    observer.calls != 0 || fixture.ram[0x44 / sizeof(u32)] != 0 ||
	    stats->executed_instructions != 0 || stats->fallback_blocks != 0 ||
	    stats->fallback_instructions != 0) {
		fputs("reordered store was falsely attributed to its source PC\n", stderr);
		fixture_destroy(&fixture);
		return 1;
	}
	fixture_destroy(&fixture);
	return 0;
}

static int test_boundary_stop_precedes_lookup_and_execution(void)
{
	struct boundary_test_context boundary = {
	    .mode = BOUNDARY_STOP_AT_PC,
	    .match_pc = 0x10,
	};
	const struct lightrec_execution_stats *stats;
	struct lightrec_registers *registers;
	struct fixture fixture;
	u32 next_pc;

	if (fixture_init_with_boundary(&fixture, &boundary))
		return 1;
	fixture.ram[0] = 0x08000004; /* j 0x10 */
	fixture.ram[1] = 0x00000000; /* nop */
	fixture.ram[4] = 0x2402002a; /* stopped: addiu $v0, $zero, 42 */
	fixture.ram[5] = 0x0000000c; /* syscall */

	next_pc = lightrec_execute(fixture.state, 0, 100);
	stats = lightrec_get_execution_stats(fixture.state);
	registers = lightrec_get_registers(fixture.state);
	if (next_pc != 0x10 || boundary.call_count != 2 || boundary.observed_pcs[0] != 0 ||
	    boundary.observed_pcs[1] != 0x10 || boundary.observed_cycles[0] != 0 ||
	    boundary.observed_cycles[1] != 4 || registers->gpr[2] != 0 ||
	    !(lightrec_exit_flags(fixture.state) & LIGHTREC_EXIT_BLOCK_BOUNDARY) ||
	    stats->translated_blocks != 1 || stats->executed_blocks != 1 ||
	    stats->executed_instructions != 2 || stats->cache_hits != 0 ||
	    stats->cache_misses != 1) {
		fputs("block-boundary stop did not precede lookup and execution\n", stderr);
		fixture_destroy(&fixture);
		return 1;
	}

	fixture_destroy(&fixture);
	return 0;
}

static int test_boundary_redirect_replaces_direct_target(void)
{
	struct boundary_test_context boundary = {
	    .mode = BOUNDARY_REDIRECT_AT_PC,
	    .match_pc = 0x10,
	    .redirect_pc = 0x20,
	};
	const struct lightrec_execution_stats *stats;
	struct lightrec_registers *registers;
	struct fixture fixture;

	if (fixture_init_with_boundary(&fixture, &boundary))
		return 1;
	fixture.ram[0] = 0x08000004; /* j 0x10 */
	fixture.ram[1] = 0x00000000; /* nop */
	fixture.ram[4] = 0x24020001; /* replaced: addiu $v0, $zero, 1 */
	fixture.ram[5] = 0x0000000c; /* syscall */
	fixture.ram[8] = 0x2402002a; /* replacement: addiu $v0, $zero, 42 */
	fixture.ram[9] = 0x0000000c; /* syscall */

	lightrec_execute(fixture.state, 0, 100);
	stats = lightrec_get_execution_stats(fixture.state);
	registers = lightrec_get_registers(fixture.state);
	if (boundary.call_count != 3 || boundary.observed_pcs[0] != 0 ||
	    boundary.observed_pcs[1] != 0x10 || boundary.observed_pcs[2] != 0x20 ||
	    boundary.observed_cycles[0] != 0 || boundary.observed_cycles[1] != 4 ||
	    boundary.observed_cycles[2] != 4 || registers->gpr[2] != 42 ||
	    stats->translated_blocks != 2 || stats->cache_misses != 2 ||
	    stats->executed_blocks != 2 || stats->executed_instructions != 4) {
		fputs("block-boundary redirect/direct-target contract failed\n", stderr);
		fixture_destroy(&fixture);
		return 1;
	}

	fixture_destroy(&fixture);
	return 0;
}

static int test_exception_context(void)
{
	const u32 exceptions[] = {0x0000000c, 0x0000000d};
	unsigned int diagnostic, exception, path;

	for (diagnostic = 0; diagnostic < 2; diagnostic++) {
		for (exception = 0; exception < 2; exception++) {
			for (path = 0; path < 4; path++) {
				struct fixture fixture;
				const struct lightrec_execution_stats *stats;
				u32 pc,
				    flags = exception ? LIGHTREC_EXIT_BREAK : LIGHTREC_EXIT_SYSCALL;
				if (fixture_init(&fixture))
					return 1;
				/* Entering the instruction directly must not inherit the
				 * preceding branch's delay-slot context. */
				fixture.ram[0] = 0x14200003; /* bne $at, $zero, 0x10 */
				fixture.ram[1] = exceptions[exception];
				fixture.ram[2] = 0x24020001; /* forbidden fallthrough */
				fixture.ram[3] = 0x0000000c;
				fixture.ram[4] = 0x24020002; /* forbidden branch target */
				fixture.ram[5] = 0x0000000c;
				lightrec_get_registers(fixture.state)->gpr[1] = path == 1;
				if (path == 3)
					fixture.ram[0] = 0x08000004; /* unconditional j 0x10 */
				if (path != 0)
					flags |= LIGHTREC_EXIT_EXCEPTION_DELAY_SLOT;
				pc =
				    diagnostic
					? lightrec_run_interpreter(fixture.state, path ? 0 : 4, 100)
					: lightrec_execute(fixture.state, path ? 0 : 4, 100);
				stats = lightrec_get_execution_stats(fixture.state);
				if (pc != 4 || lightrec_exit_flags(fixture.state) != flags ||
				    lightrec_current_cycle_count(fixture.state) !=
					(path ? 4u : 2u) ||
				    lightrec_get_registers(fixture.state)->gpr[2] != 0 ||
				    (!diagnostic &&
				     (!stats->executed_blocks || stats->fallback_blocks))) {
					fprintf(stderr,
						"exception context failed: diagnostic=%u "
						"exception=%u path=%u pc=%x flags=%x expected=%x\n",
						diagnostic, exception, path, pc,
						lightrec_exit_flags(fixture.state), flags);
					fixture_destroy(&fixture);
					return 1;
				}
				fixture_destroy(&fixture);
			}
		}
	}
	puts("exception context: JIT and diagnostic SYSCALL/BREAK, direct/taken/untaken/jump "
	     "passed");
	return 0;
}

int main(void)
{
	if (test_exception_context())
		return 1;
	if (test_cold_block_jits_without_fallback())
		return 1;
	if (test_unsupported_block_has_typed_fallback())
		return 1;
	if (test_fallback_refusal_executes_no_interpreter_instructions())
		return 1;
	if (test_allowed_fallback_resumes_dynarec())
		return 1;
	if (test_unsafe_fetch_is_not_silent())
		return 1;
	if (test_unsafe_fetch_refusal_precedes_native_fault())
		return 1;
	if (test_diagnostic_interpreter_is_explicit_and_unmixed())
		return 1;
	if (test_boundary_callback_observes_cache_hit())
		return 1;
	if (test_selected_interior_store_observer())
		return 1;
	if (test_reordered_store_observer_refuses_false_pc())
		return 1;
	if (test_boundary_stop_precedes_lookup_and_execution())
		return 1;
	if (test_boundary_redirect_replaces_direct_target())
		return 1;
	return 0;
}
