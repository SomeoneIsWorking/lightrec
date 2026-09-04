/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef __LIGHTREC_EXECUTION_H__
#define __LIGHTREC_EXECUTION_H__

#include "lightrec-private.h"

#include <stdbool.h>

bool lightrec_run_block_boundary(struct lightrec_state *state, u32 pc);
void lightrec_emit_increment_dispatch_counter(jit_state_t *_jit, size_t counter_offset);
void lightrec_print_info(struct lightrec_state *state);

#endif /* __LIGHTREC_EXECUTION_H__ */
