// SPDX-License-Identifier: LGPL-2.1-or-later
/* Copyright (C) 2014-2021 Paul Cercueil <paul@crapouillou.net> */
/* Delay-slot scheduling preserves register dependencies and exception context. */

#include "debug.h"
#include "lightrec-private.h"
#include "optimizer.h"

#include <stdbool.h>

_Bool lightrec_can_switch_delay_slot(union code op, union code next_op)
{
	/* Traps must execute after the branch establishes its architectural context. */
	if (next_op.i.op == OP_SPECIAL &&
	    (next_op.r.op == OP_SPECIAL_SYSCALL || next_op.r.op == OP_SPECIAL_BREAK))
		return false;

	switch (op.i.op) {
	case OP_SPECIAL:
		switch (op.r.op) {
		case OP_SPECIAL_JALR:
			if (opcode_reads_register(next_op, op.r.rd) ||
			    opcode_writes_register(next_op, op.r.rd))
				return false;
			fallthrough;
		case OP_SPECIAL_JR:
			if (opcode_writes_register(next_op, op.r.rs))
				return false;
			fallthrough;
		default:
			break;
		}
		fallthrough;
	case OP_J:
		break;
	case OP_JAL:
		if (opcode_reads_register(next_op, 31) || opcode_writes_register(next_op, 31))
			return false;

		break;
	case OP_BEQ:
	case OP_BNE:
		if (op.i.rt && opcode_writes_register(next_op, op.i.rt))
			return false;
		fallthrough;
	case OP_BLEZ:
	case OP_BGTZ:
		if (op.i.rs && opcode_writes_register(next_op, op.i.rs))
			return false;
		break;
	case OP_REGIMM:
		switch (op.r.rt) {
		case OP_REGIMM_BLTZAL:
		case OP_REGIMM_BGEZAL:
			if (opcode_reads_register(next_op, 31) ||
			    opcode_writes_register(next_op, 31))
				return false;
			fallthrough;
		case OP_REGIMM_BLTZ:
		case OP_REGIMM_BGEZ:
			if (op.i.rs && opcode_writes_register(next_op, op.i.rs))
				return false;
			break;
		}
		fallthrough;
	default:
		break;
	}

	return true;
}

int lightrec_switch_delay_slots(struct lightrec_state *state, struct block *block)
{
	struct opcode *list, *next = &block->opcode_list[0];
	unsigned int i;
	union code op, next_op;
	u32 flags, source_pc;

	for (i = 0; i < block->nb_ops - 1; i++) {
		list = next;
		next = &block->opcode_list[i + 1];
		next_op = next->c;
		op = list->c;

		if (!has_delay_slot(op) || op_flag_no_ds(list->flags) ||
		    op_flag_emulate_branch(list->flags) || op.opcode == 0 || next_op.opcode == 0)
			continue;

		if (is_delay_slot(block->opcode_list, i))
			continue;

		if (op_flag_sync(next->flags))
			continue;

		if (op_flag_load_delay(next->flags) && opcode_has_load_delay(next_op)) {
			continue;
		}

		if (!lightrec_can_switch_delay_slot(list->c, next_op))
			continue;

		pr_debug("Swap branch and delay slot opcodes "
			 "at offsets 0x%x / 0x%x\n",
			 i << 2, (i + 1) << 2);

		flags = next->flags | (list->flags & LIGHTREC_SYNC);
		source_pc = list->source_pc;
		list->c = next_op;
		next->c = op;
		list->source_pc = next->source_pc;
		next->source_pc = source_pc;
		next->flags = (list->flags | LIGHTREC_NO_DS) & ~LIGHTREC_SYNC;
		list->flags = flags | LIGHTREC_NO_DS;
	}

	return 0;
}
