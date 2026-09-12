// SPDX-License-Identifier: LGPL-2.1-or-later
/* Diagnostic codegen for selected, provenance-preserved translated SW instructions. */

#include "store-observer.h"

#include "disassembler.h"
#include "execution.h"
#include "lightrec-private.h"
#include "regcache.h"

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
