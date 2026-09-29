// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * Shared dynarec test fixture.
 *
 * The contract tests all need the same synthetic machine: a two-megabyte guest
 * RAM, a 512 KiB BIOS, a 1 KiB scratchpad, and the two ops the emulator must
 * supply before it will translate anything. It lives here so every test binary
 * builds the same machine instead of copying the map.
 */

#ifndef __DYNAREC_FIXTURE_H__
#define __DYNAREC_FIXTURE_H__

#include "lightrec.h"

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

static inline void fixture_cop2_op(struct lightrec_state *state, u32 opcode)
{
	(void)state;
	(void)opcode;
}

static inline void fixture_enable_ram(struct lightrec_state *state, _Bool enable)
{
	(void)state;
	(void)enable;
}

static inline void fixture_destroy(struct fixture *fixture)
{
	if (fixture->state)
		lightrec_destroy(fixture->state);
	free(fixture->scratch);
	free(fixture->bios);
	free(fixture->ram);
	*fixture = (struct fixture){0};
}

/* The caller owns ops and it must outlive the fixture. */
static inline int fixture_init_ops(struct fixture *fixture, char *argv0,
				   const struct lightrec_ops *ops)
{
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

	fixture->state = lightrec_init(argv0, fixture->maps, PSX_MAP_CODE_BUFFER + 1, ops);
	if (!fixture->state) {
		fixture_destroy(fixture);
		return -1;
	}
	return 0;
}

static inline int fixture_init(struct fixture *fixture, char *argv0)
{
	const struct lightrec_ops ops = {
	    .cop2_op = fixture_cop2_op,
	    .enable_ram = fixture_enable_ram,
	};

	return fixture_init_ops(fixture, argv0, &ops);
}

#endif /* __DYNAREC_FIXTURE_H__ */
