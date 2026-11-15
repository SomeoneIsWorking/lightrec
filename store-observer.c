// SPDX-License-Identifier: LGPL-2.1-or-later
/* Observers of selected translated SW instructions: registration, admission and codegen. */

#include "store-observer.h"

#include "blockcache.h"
#include "disassembler.h"
#include "execution.h"
#include "lightrec-private.h"
#include "optimizer.h"
#include "regcache.h"

#include <errno.h>
#include <stdbool.h>
#include <string.h>

void lightrec_rec_observed_store(struct lightrec_cstate *state, const struct block *block,
				 u16 offset, enum lightrec_store_observer_phase phase)
{
	struct regcache *reg_cache = state->reg_cache;
	jit_state_t *_jit = block->_jit;
	u32 op_cycles = lightrec_cycles_of_opcode(state->state, block->opcode_list[offset].c);
	u32 pending = state->cycles + (phase == LIGHTREC_STORE_AFTER ? op_cycles : 0);

	if (pending)
		jit_subi(LIGHTREC_REG_CYCLE, LIGHTREC_REG_CYCLE, pending);
	state->cycles = phase == LIGHTREC_STORE_AFTER ? -op_cycles : 0;
	lightrec_clean_regs(reg_cache, _jit);
	lightrec_regcache_reset(reg_cache);
	lightrec_update_cycle_counter_before_c(_jit);

	jit_prepare();
	jit_pushargr(LIGHTREC_REG_STATE);
	jit_pushargi(block->pc + (offset << 2));
	jit_pushargi(phase);
	jit_finishi(lightrec_notify_store_observer);

	lightrec_update_cycle_counter_after_c(_jit);
}

bool lightrec_observed_block_unsupported(const struct lightrec_state *state,
					 const struct block *block)
{
	unsigned int i;
	union code previous;
	u32 guest_pc, raw;

	if (!state->store_observer || block_has_flag(block, BLOCK_NO_OPCODE_LIST))
		return false;
	for (i = 0; i < block->nb_ops; i++) {
		guest_pc = block->pc + (i << 2);
		if (!lightrec_store_observer_matches(state, guest_pc))
			continue;
		raw = LE32TOH(block->code[i]);
		previous.opcode = i ? LE32TOH(block->code[i - 1]) : 0;
		if (block_has_flag(block, BLOCK_NEVER_COMPILE) ||
		    block_has_flag(block, BLOCK_IS_MEMSET) || (raw >> 26) != OP_SW ||
		    block->opcode_list[i].opcode != raw ||
		    block->opcode_list[i].source_pc != guest_pc ||
		    (i && has_delay_slot(previous)) || is_delay_slot(block->opcode_list, i) ||
		    should_emulate(&block->opcode_list[i]))
			return true;
	}
	return false;
}

int lightrec_set_store_observer(struct lightrec_state *state, const u32 *targets, size_t count,
				lightrec_store_observer_cb callback, void *user_data)
{
	size_t i, j;

	if (!state || (count == 0 && (targets || callback || user_data)) ||
	    (count != 0 && (!targets || !callback || count > LIGHTREC_STORE_OBSERVER_TARGETS)))
		return -EINVAL;
	if (state->executing)
		return -EBUSY;
	for (i = 0; i < count; i++) {
		if (targets[i] & 3)
			return -EINVAL;
		for (j = 0; j < i; j++) {
			if (targets[i] == targets[j])
				return -EINVAL;
		}
	}

	/* A LUT-only invalidation would reuse the old unchanged compiled block. */
	lightrec_invalidate_all(state);
	lightrec_free_all_blocks(state->block_cache);
	state->store_observer = callback;
	state->store_observer_data = user_data;
	state->store_observer_target_count = count;
	if (count)
		memcpy(state->store_observer_targets, targets, count * sizeof(*targets));
	return 0;
}

void lightrec_notify_store_observer(struct lightrec_state *state, u32 guest_pc,
				    enum lightrec_store_observer_phase phase)
{
	state->store_observer(&state->regs, guest_pc, phase, state->current_cycle,
			      state->store_observer_data);
}
