// SPDX-License-Identifier: LGPL-2.1-or-later
/* Copyright (C) 2014-2021 Paul Cercueil <paul@crapouillou.net> */
/* Delay-slot scheduling preserves register dependencies and exception context. */

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
