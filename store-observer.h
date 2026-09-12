// SPDX-License-Identifier: LGPL-2.1-or-later

#ifndef __STORE_OBSERVER_H__
#define __STORE_OBSERVER_H__

#include "lightrec.h"

struct block;
struct lightrec_cstate;

void lightrec_rec_observed_store(struct lightrec_cstate *state, const struct block *block,
				 u16 offset, enum lightrec_store_observer_phase phase);

#endif /* __STORE_OBSERVER_H__ */
