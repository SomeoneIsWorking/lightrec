// SPDX-License-Identifier: LGPL-2.1-or-later

#ifndef __STORE_OBSERVER_H__
#define __STORE_OBSERVER_H__

#include "lightrec.h"

#include <stdbool.h>

struct block;
struct lightrec_cstate;
struct lightrec_state;

void lightrec_rec_observed_store(struct lightrec_cstate *state, const struct block *block,
				 u16 offset, enum lightrec_store_observer_phase phase);
/* A block with an observed SW the translator cannot preserve exactly. */
bool lightrec_observed_block_unsupported(const struct lightrec_state *state,
					 const struct block *block);

#endif /* __STORE_OBSERVER_H__ */
