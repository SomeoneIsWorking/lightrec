/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Copyright (C) 2016-2021 Paul Cercueil <paul@crapouillou.net>
 */

#ifndef __LIGHTREC_H__
#define __LIGHTREC_H__

#ifdef __cplusplus
#define _Bool bool
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#   ifdef lightrec_EXPORTS
#	define __api __declspec(dllexport)
#   elif !defined(LIGHTREC_STATIC)
#	define __api __declspec(dllimport)
#   else
#	define __api
#   endif
#elif __GNUC__ >= 4
#   define __api __attribute__((visibility ("default")))
#else
#   define __api
#endif

#ifndef __cnst
#   ifdef __GNUC__
#	define __cnst __attribute__((const))
#   else
#	define __cnst
#   endif
#endif
#ifndef __pure
#   ifdef __GNUC__
#	define __pure __attribute__((pure))
#   else
#	define __pure
#   endif
#endif

typedef uint64_t u64;
typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t  u8;

typedef int64_t s64;
typedef int32_t s32;
typedef int16_t s16;
typedef int8_t  s8;

struct lightrec_state;
struct lightrec_mem_map;

enum lightrec_block_boundary_action {
	LIGHTREC_BLOCK_CONTINUE = 0,
	LIGHTREC_BLOCK_STOP,
	LIGHTREC_BLOCK_REDIRECT,
};

typedef enum lightrec_block_boundary_action (*lightrec_block_boundary_cb)(
    struct lightrec_state *state, u32 guest_pc, u32 *redirect_pc, void *user_data);

enum lightrec_fallback_reason {
	LIGHTREC_FALLBACK_NONE = 0,
	LIGHTREC_FALLBACK_SELF_MODIFYING_CODE,
	LIGHTREC_FALLBACK_UNSUPPORTED_CONTROL_FLOW,
	LIGHTREC_FALLBACK_JIT_COMPILE_FAILURE,
	LIGHTREC_FALLBACK_LOAD_DELAY_HAZARD,
	LIGHTREC_FALLBACK_UNSAFE_FETCH,
	LIGHTREC_FALLBACK_REASON_COUNT,
};

struct lightrec_fallback_event {
	enum lightrec_fallback_reason reason;
	u32 guest_pc;
	int host_error;
};

enum lightrec_fallback_action {
	LIGHTREC_FALLBACK_ALLOW = 0,
	LIGHTREC_FALLBACK_REFUSE,
};

typedef enum lightrec_fallback_action (*lightrec_fallback_admission_cb)(
    struct lightrec_state *state, const struct lightrec_fallback_event *event, void *user_data);

struct lightrec_execution_stats {
	union {
		u64 executed_blocks;
		u64 jit_blocks; /* Compatibility alias. */
	};
	union {
		u64 executed_instructions;
		u64 jit_instructions; /* Compatibility alias. */
	};
	u64 fallback_blocks;
	u64 fallback_instructions;
	u64 fallback_blocks_by_reason[LIGHTREC_FALLBACK_REASON_COUNT];
	u64 fallback_instructions_by_reason[LIGHTREC_FALLBACK_REASON_COUNT];
	u64 refused_fallback_blocks;
	u64 refused_fallback_blocks_by_reason[LIGHTREC_FALLBACK_REASON_COUNT];
	u64 translated_blocks;
	u64 translated_instructions;
	u64 cache_hits;
	u64 cache_misses;
};

/* Exit flags */
#define LIGHTREC_EXIT_NORMAL	(0)
#define LIGHTREC_EXIT_CHECK_INTERRUPT	(1 << 0)
#define LIGHTREC_EXIT_BREAK	(1 << 1)
#define LIGHTREC_EXIT_SYSCALL	(1 << 2)
#define LIGHTREC_EXIT_SEGFAULT	(1 << 3)
#define LIGHTREC_EXIT_NOMEM	(1 << 4)
#define LIGHTREC_EXIT_UNKNOWN_OP	(1 << 5)
#define LIGHTREC_EXIT_BLOCK_BOUNDARY (1 << 6)
#define LIGHTREC_EXIT_FALLBACK_REFUSED (1 << 7)
/* Qualifies SYSCALL/BREAK: the returned PC is the trapping instruction,
 * executed in a branch delay slot. Consumers own EPC/BD and resumption policy. */
#define LIGHTREC_EXIT_EXCEPTION_DELAY_SLOT (1 << 8)

/* Unsafe optimizations flags */
#define LIGHTREC_OPT_INV_DMA_ONLY	(1 << 0)
#define LIGHTREC_OPT_SP_GP_HIT_RAM	(1 << 1)

enum psx_map {
	PSX_MAP_KERNEL_USER_RAM,
	PSX_MAP_BIOS,
	PSX_MAP_SCRATCH_PAD,
	PSX_MAP_PARALLEL_PORT,
	PSX_MAP_HW_REGISTERS,
	PSX_MAP_CACHE_CONTROL,
	PSX_MAP_MIRROR1,
	PSX_MAP_MIRROR2,
	PSX_MAP_MIRROR3,
	PSX_MAP_PPORT_MIRROR,
	PSX_MAP_BIOS_MIRROR1,
	PSX_MAP_BIOS_MIRROR2,
	PSX_MAP_BIOS_MIRROR3,
	PSX_MAP_BIOS_MIRROR4,
	PSX_MAP_BIOS_MIRROR5,
	PSX_MAP_BIOS_MIRROR6,
	PSX_MAP_BIOS_MIRROR7,

	PSX_MAP_CODE_BUFFER, /* should be last */

	PSX_MAP_UNKNOWN,
};

struct lightrec_mem_map_ops {
	void (*sb)(struct lightrec_state *, u32 opcode,
		   void *host, u32 addr, u32 data);
	void (*sh)(struct lightrec_state *, u32 opcode,
		   void *host, u32 addr, u32 data);
	void (*sw)(struct lightrec_state *, u32 opcode,
		   void *host, u32 addr, u32 data);
	u8 (*lb)(struct lightrec_state *, u32 opcode, void *host, u32 addr);
	u16 (*lh)(struct lightrec_state *, u32 opcode, void *host, u32 addr);
	u32 (*lw)(struct lightrec_state *, u32 opcode, void *host, u32 addr);
	u32 (*lwu)(struct lightrec_state *, u32 opcode, void *host, u32 addr);
	void (*swu)(struct lightrec_state *, u32 opcode,
		    void *host, u32 addr, u32 data);
};

struct lightrec_mem_map {
	u32 pc;
	u32 length;
	void *address;
	const struct lightrec_mem_map_ops *ops;
	const struct lightrec_mem_map *mirror_of;
};

struct lightrec_ops {
	void (*cop2_notify)(struct lightrec_state *state, u32 op, u32 data);
	void (*cop2_op)(struct lightrec_state *state, u32 op);
	void (*enable_ram)(struct lightrec_state *state, _Bool enable);
	_Bool (*hw_direct)(u32 kaddr, _Bool is_write, u8 size);
	void (*code_inv)(void *addr, u32 len);
	lightrec_block_boundary_cb block_boundary;
	void *block_boundary_data;
	lightrec_fallback_admission_cb fallback_admission;
	void *fallback_admission_data;
};

struct lightrec_registers {
	u32 gpr[34];
	u32 cp0[32];
	u32 cp2d[32];
	u32 cp2c[32];
};

__api struct lightrec_state *lightrec_init(char *argv0,
					   const struct lightrec_mem_map *map,
					   size_t nb,
					   const struct lightrec_ops *ops);

__api void lightrec_destroy(struct lightrec_state *state);

__api u32 lightrec_execute(struct lightrec_state *state,
			   u32 pc, u32 target_cycle);

/* Diagnostic-only execution mode. Product gameplay uses lightrec_execute(). */
__api u32 lightrec_run_interpreter(struct lightrec_state *state,
				   u32 pc, u32 target_cycle);

__api const struct lightrec_execution_stats *
lightrec_get_execution_stats(const struct lightrec_state *state);
__api void lightrec_reset_execution_stats(struct lightrec_state *state);
__api const struct lightrec_fallback_event *
lightrec_get_last_fallback(const struct lightrec_state *state);
__api const char *lightrec_fallback_reason_name(enum lightrec_fallback_reason reason);
__api _Bool lightrec_execution_is_dynarec_dominated(const struct lightrec_state *state);

__api void lightrec_invalidate(struct lightrec_state *state, u32 addr, u32 len);
__api void lightrec_invalidate_all(struct lightrec_state *state);

__api void lightrec_set_exit_flags(struct lightrec_state *state, u32 flags);
__api u32 lightrec_exit_flags(struct lightrec_state *state);

__api void lightrec_set_unsafe_opt_flags(struct lightrec_state *state, u32 flags);

__api __cnst struct lightrec_registers *
lightrec_get_registers(struct lightrec_state *state);

__api u32 lightrec_current_cycle_count(const struct lightrec_state *state);
__api void lightrec_reset_cycle_count(struct lightrec_state *state, u32 cycles);
__api void lightrec_set_target_cycle_count(struct lightrec_state *state,
					   u32 cycles);
__api void lightrec_set_cycles_per_opcode(struct lightrec_state *state, u32 cycles);

#ifdef __cplusplus
};
#endif

#endif /* __LIGHTREC_H__ */
