// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * Copyright (C) 2014-2021 Paul Cercueil <paul@crapouillou.net>
 */

#include "execution.h"

#include "blockcache.h"
#include "debug.h"
#include "memmanager.h"

#include <limits.h>
#include <string.h>

bool lightrec_run_block_boundary(struct lightrec_state *state, u32 pc)
{
	enum lightrec_block_boundary_action action;
	u32 redirect_pc = pc;

	action = state->ops.block_boundary(state, pc, &redirect_pc, state->ops.block_boundary_data);
	switch (action) {
	case LIGHTREC_BLOCK_CONTINUE:
		return true;
	case LIGHTREC_BLOCK_REDIRECT:
		state->curr_pc = redirect_pc;
		state->next_pc = redirect_pc;
		return false;
	case LIGHTREC_BLOCK_STOP:
	default:
		state->curr_pc = pc;
		lightrec_set_exit_flags(state, LIGHTREC_EXIT_BLOCK_BOUNDARY);
		return false;
	}
}

void lightrec_emit_increment_dispatch_counter(jit_state_t *_jit, size_t counter_offset)
{
	jit_ldxi_l(JIT_R1, LIGHTREC_REG_STATE, counter_offset);
	jit_addi(JIT_R1, JIT_R1, 1);
	jit_stxi_l(counter_offset, LIGHTREC_REG_STATE, JIT_R1);
}

void lightrec_print_info(struct lightrec_state *state)
{
	if ((state->current_cycle & ~0xfffffff) != state->old_cycle_counter) {
		pr_info("Lightrec RAM usage: IR %u KiB, CODE %u KiB, "
			"MIPS %u KiB, TOTAL %u KiB, avg. IPI %f\n",
			lightrec_get_mem_usage(MEM_FOR_IR) / 1024,
			lightrec_get_mem_usage(MEM_FOR_CODE) / 1024,
			lightrec_get_mem_usage(MEM_FOR_MIPS_CODE) / 1024,
			lightrec_get_total_mem_usage() / 1024, lightrec_get_average_ipi());
		state->old_cycle_counter = state->current_cycle & ~0xfffffff;
	}
}

u32 lightrec_execute(struct lightrec_state *state, u32 pc, u32 target_cycle)
{
	s32 (*func)(struct lightrec_state *, u32, void *, s32) =
	    (void *)state->dispatcher->function;
	s32 cycles_delta;

	state->exit_flags = LIGHTREC_EXIT_NORMAL;
	if (unlikely(target_cycle < state->current_cycle))
		target_cycle = UINT_MAX;
	state->target_cycle = target_cycle;
	state->curr_pc = pc;

	if (state->current_cycle < state->target_cycle) {
		cycles_delta = state->target_cycle - state->current_cycle;
		cycles_delta = (*func)(state, state->curr_pc, NULL, cycles_delta);
		state->current_cycle = state->target_cycle - cycles_delta;
	}
	if (LOG_LEVEL >= INFO_L)
		lightrec_print_info(state);
	return state->curr_pc;
}

void lightrec_set_exit_flags(struct lightrec_state *state, u32 flags)
{
	if (flags != LIGHTREC_EXIT_NORMAL) {
		state->exit_flags |= flags;
		state->target_cycle = state->current_cycle;
	}
}

u32 lightrec_exit_flags(struct lightrec_state *state)
{
	return state->exit_flags;
}

const struct lightrec_execution_stats *
lightrec_get_execution_stats(const struct lightrec_state *state)
{
	return &state->execution_stats;
}

void lightrec_reset_execution_stats(struct lightrec_state *state)
{
	memset(&state->execution_stats, 0, sizeof(state->execution_stats));
	memset(&state->last_fallback, 0, sizeof(state->last_fallback));
}

const struct lightrec_fallback_event *lightrec_get_last_fallback(const struct lightrec_state *state)
{
	return &state->last_fallback;
}

const char *lightrec_fallback_reason_name(enum lightrec_fallback_reason reason)
{
	static const char *const names[LIGHTREC_FALLBACK_REASON_COUNT] = {
	    [LIGHTREC_FALLBACK_NONE] = "none",
	    [LIGHTREC_FALLBACK_SELF_MODIFYING_CODE] = "self_modifying_code",
	    [LIGHTREC_FALLBACK_UNSUPPORTED_CONTROL_FLOW] = "unsupported_control_flow",
	    [LIGHTREC_FALLBACK_JIT_COMPILE_FAILURE] = "jit_compile_failure",
	    [LIGHTREC_FALLBACK_LOAD_DELAY_HAZARD] = "load_delay_hazard",
	    [LIGHTREC_FALLBACK_UNSAFE_FETCH] = "unsafe_fetch",
	};

	if ((unsigned int)reason >= LIGHTREC_FALLBACK_REASON_COUNT)
		return "invalid";
	return names[reason];
}

_Bool lightrec_execution_is_dynarec_dominated(const struct lightrec_state *state)
{
	return state->execution_stats.executed_instructions >
	       state->execution_stats.fallback_instructions;
}

u32 lightrec_current_cycle_count(const struct lightrec_state *state)
{
	return state->current_cycle;
}

void lightrec_reset_cycle_count(struct lightrec_state *state, u32 cycles)
{
	state->current_cycle = cycles;
	if (state->target_cycle < cycles)
		state->target_cycle = cycles;
}

void lightrec_set_target_cycle_count(struct lightrec_state *state, u32 cycles)
{
	if (state->exit_flags == LIGHTREC_EXIT_NORMAL) {
		if (cycles < state->current_cycle)
			cycles = state->current_cycle;
		state->target_cycle = cycles;
	}
}

struct lightrec_registers *lightrec_get_registers(struct lightrec_state *state)
{
	return &state->regs;
}

void lightrec_set_cycles_per_opcode(struct lightrec_state *state, u32 cycles)
{
	if (state->cycles_per_op == cycles)
		return;
	state->cycles_per_op = cycles;
	lightrec_invalidate_all(state);
	lightrec_free_all_blocks(state->block_cache);
}
