// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * Copyright (C) 2015-2021 Paul Cercueil <paul@crapouillou.net>
 */

#include "blockcache.h"
#include "debug.h"
#include "lightrec-private.h"
#include "memmanager.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* Must be power of two */
#define LUT_SIZE 0x4000

/* One bit per guest code word, indexed by lut_offset() so that RAM and the BIOS
 * share one compact index exactly as the code LUT does. A set bit means a block
 * was registered over that word; a bit is never cleared, so a word that only
 * ever held translated code can cost one wasted walk later. This is what keeps
 * a store into a region with nothing translated down to a handful of
 * instructions, on a path that runs for every guest store. */
#define TRANSLATED_WORDS_SIZE (CODE_LUT_SIZE / 8)

struct blockcache {
	struct lightrec_state *state;
	struct block *lut[LUT_SIZE];
	u8 translated_words[TRANSLATED_WORDS_SIZE];
};

/* The length of a word run that still fits the compact code index. A block at
 * the top of the BIOS image can be disassembled past the end of the image, so
 * the run is bounded here rather than trusted from the caller. */
static inline u32 clamp_word_run(u32 offset, u32 count)
{
	return count > CODE_LUT_SIZE - offset ? CODE_LUT_SIZE - offset : count;
}

static void mark_translated_words(struct blockcache *cache, u32 offset, u32 count)
{
	u32 i, words = clamp_word_run(offset, count);

	for (i = 0; i < words; i++) {
		u32 word = offset + i;

		cache->translated_words[word >> 3] |= 1 << (word & 7);
	}
}

static _Bool any_translated_word(const struct blockcache *cache, u32 offset, u32 count)
{
	u32 i, words = clamp_word_run(offset, count);

	for (i = 0; i < words; i++) {
		u32 word = offset + i;

		if (cache->translated_words[word >> 3] & (1 << (word & 7)))
			return true;
	}

	return false;
}

u16 lightrec_get_lut_entry(const struct block *block)
{
	return (kunseg(block->pc) >> 2) & (LUT_SIZE - 1);
}

struct block *lightrec_find_block(struct blockcache *cache, u32 pc)
{
	struct block *block;

	pc = kunseg(pc);

	for (block = cache->lut[(pc >> 2) & (LUT_SIZE - 1)]; block; block = block->next)
		if (kunseg(block->pc) == pc)
			return block;

	return NULL;
}

struct block *lightrec_find_block_from_lut(struct blockcache *cache, u16 lut_entry,
					   u32 addr_in_block)
{
	struct block *block;
	u32 pc;

	addr_in_block = kunseg(addr_in_block);

	for (block = cache->lut[lut_entry]; block; block = block->next) {
		pc = kunseg(block->pc);
		if (addr_in_block >= pc && addr_in_block < block_end_pc(block))
			return block;
	}

	return NULL;
}

void remove_from_code_lut(struct blockcache *cache, struct block *block)
{
	struct lightrec_state *state = cache->state;
	u32 offset = lut_offset(block->pc);

	if (block->function) {
		memset(lut_address(state, offset), 0, block->nb_ops * lut_elm_size(state));
	}
}

void lightrec_register_block(struct blockcache *cache, struct block *block)
{
	u32 pc = kunseg(block->pc);
	struct block *old;

	old = cache->lut[(pc >> 2) & (LUT_SIZE - 1)];
	if (old)
		block->next = old;

	cache->lut[(pc >> 2) & (LUT_SIZE - 1)] = block;

	mark_translated_words(cache, lut_offset(pc), block->nb_ops);
	remove_from_code_lut(cache, block);
}

void lightrec_unregister_block(struct blockcache *cache, struct block *block)
{
	u32 pc = kunseg(block->pc);
	struct block *old = cache->lut[(pc >> 2) & (LUT_SIZE - 1)];

	if (old == block) {
		cache->lut[(pc >> 2) & (LUT_SIZE - 1)] = old->next;
		return;
	}

	for (; old; old = old->next) {
		if (old->next == block) {
			old->next = block->next;
			return;
		}
	}

	pr_err("Block at " PC_FMT " is not in cache\n", block->pc);
}

static bool lightrec_block_is_old(const struct lightrec_state *state, const struct block *block)
{
	u32 diff = state->current_cycle - block->precompile_date;

	return diff > (1 << 27); /* About 4 seconds */
}

/* One pass over every registered block. Reclamation and range invalidation
 * differ in which blocks they select and in what they do to a selected block,
 * not in how they walk the cache, so both go through here and cannot drift
 * apart. A pass reports how many blocks it examined as well as how many it
 * acted on, so a caller can show that its selection was reached instead of
 * assuming a walk that matched nothing had nothing to match. */
struct block_walk {
	_Bool (*selects)(struct block *block, const void *arg);
	_Bool (*applies)(struct blockcache *cache, struct block *block, void *arg);
	const void *select_arg;
	void *apply_arg;
	unsigned int examined;
	unsigned int changed;
};

static void lightrec_walk_blocks(struct blockcache *cache, struct block_walk *walk)
{
	struct block *block, *next;
	unsigned int i;

	for (i = 0; i < LUT_SIZE; i++) {
		for (block = cache->lut[i]; block; block = next) {
			next = block->next;
			walk->examined++;

			if (!walk->selects(block, walk->select_arg))
				continue;

			if (walk->applies(cache, block, walk->apply_arg))
				walk->changed++;
		}
	}
}

struct block_reclaim {
	struct lightrec_state *state;
	const struct block *except;
	_Bool all;
};

static _Bool lightrec_block_is_reclaimable(struct block *block, const void *arg)
{
	const struct block_reclaim *reclaim = arg;

	if (reclaim->all)
		return true;
	if (block == reclaim->except)
		return false;

	return lightrec_block_is_old(reclaim->state, block) ||
	       lightrec_block_is_outdated(reclaim->state, block);
}

static _Bool lightrec_reclaim_block(struct blockcache *cache, struct block *block, void *arg)
{
	u8 old_flags = block_set_flags(block, BLOCK_IS_DEAD);

	(void)arg;

	if (old_flags & BLOCK_IS_DEAD)
		return false;

	pr_debug("Freeing outdated block at " PC_FMT "\n", block->pc);
	remove_from_code_lut(cache, block);
	lightrec_unregister_block(cache, block);
	lightrec_free_block(cache->state, block);
	return true;
}

static void lightrec_free_blocks(struct blockcache *cache, const struct block *except, bool all)
{
	struct block_reclaim reclaim = {
	    .state = cache->state,
	    .except = except,
	    .all = all,
	};
	struct block_walk walk = {
	    .selects = lightrec_block_is_reclaimable,
	    .applies = lightrec_reclaim_block,
	    .select_arg = &reclaim,
	};

	lightrec_walk_blocks(cache, &walk);
}

struct changed_range {
	u32 begin;
	u32 end;
};

static _Bool lightrec_block_overlaps_range(struct block *block, const void *arg)
{
	const struct changed_range *range = arg;

	return block_overlaps_range(block, range->begin, range->end);
}

static _Bool lightrec_revoke_block(struct blockcache *cache, struct block *block, void *arg)
{
	struct lightrec_state *state = cache->state;

	(void)arg;

	/* An uncompiled block has no translated code to revoke, and a block whose
	 * code-LUT span is already clear is already revoked, so a repeated write
	 * of the same range does no work and is not counted twice.
	 *
	 * The block keeps its identity and its memory: dropping its whole
	 * code-LUT span forces the next entry to it through
	 * lightrec_block_is_outdated(), which restores the entry when the guest
	 * bytes are unchanged and recompiles the block when they are not. The
	 * span is the whole extent, not the first word, because a guest branch
	 * can enter the middle of a block through an interior label. Nothing is
	 * freed here, which is what makes this safe to call from inside a
	 * running block, where a self-modifying store reaches it. */
	if (!block->function || !lut_read(state, lut_offset(block->pc)))
		return false;

	remove_from_code_lut(cache, block);
	return true;
}

void lightrec_invalidate_blocks(struct blockcache *cache, u32 addr, u32 len)
{
	struct lightrec_execution_stats *stats = &cache->state->execution_stats;
	struct changed_range range = {.begin = addr};
	struct block_walk walk = {
	    .selects = lightrec_block_overlaps_range,
	    .applies = lightrec_revoke_block,
	};
	u32 first, count;

	stats->invalidations++;

	/* The length can come from a guest register - lightrec_memset() reads the
	 * caller's word count - so it is bounded by the RAM window before any
	 * arithmetic is done with it. */
	if (len > RAM_SIZE)
		len = RAM_SIZE;

	first = lut_offset(addr);
	count = clamp_word_run(first, (len + 3) / 4);
	stats->invalidation_words += count;

	if (!count || !any_translated_word(cache, first, count)) {
		stats->invalidation_guards++;
		return;
	}

	range.end = addr + len;
	walk.select_arg = &range;

	lightrec_walk_blocks(cache, &walk);

	stats->invalidation_scans += walk.examined;
	stats->invalidated_blocks += walk.changed;
}

void lightrec_remove_outdated_blocks(struct blockcache *cache, const struct block *except)
{
	pr_info("Running out of code space. Cleaning block cache...\n");

	lightrec_free_blocks(cache, except, false);
}

void lightrec_free_all_blocks(struct blockcache *cache)
{
	lightrec_free_blocks(cache, NULL, true);
}

void lightrec_free_block_cache(struct blockcache *cache)
{
	lightrec_free_all_blocks(cache);
	lightrec_free(cache->state, MEM_FOR_LIGHTREC, sizeof(*cache), cache);
}

struct blockcache *lightrec_blockcache_init(struct lightrec_state *state)
{
	struct blockcache *cache;

	cache = lightrec_calloc(state, MEM_FOR_LIGHTREC, sizeof(*cache));
	if (!cache)
		return NULL;

	cache->state = state;

	return cache;
}

u32 lightrec_calculate_block_hash(const struct block *block)
{
	const u32 *code = block->code;
	u32 hash = 0xffffffff;
	unsigned int i;

	/* Jenkins one-at-a-time hash algorithm */
	for (i = 0; i < block->nb_ops; i++) {
		hash += *code++;
		hash += (hash << 10);
		hash ^= (hash >> 6);
	}

	hash += (hash << 3);
	hash ^= (hash >> 11);
	hash += (hash << 15);

	return hash;
}

bool lightrec_block_is_outdated(struct lightrec_state *state, struct block *block)
{
	u32 offset = lut_offset(block->pc);
	bool outdated;

	if (lut_read(state, offset))
		return false;

	outdated = block->hash != lightrec_calculate_block_hash(block);
	if (likely(!outdated)) {
		/* The block was marked as outdated, but the content is still
		 * the same */

		if (block->function) {
			lut_write(state, offset, block->function);
		} else {
			lut_write(state, offset, state->get_next_block);
		}
	}

	return outdated;
}
