// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * Ranged-invalidation cost probe.
 *
 * lightrec_invalidate() runs for every guest store, so the interesting numbers
 * are the two paths separately: a store into a word no block covers, which must
 * stay proportional to the store, and a store that does hit a translated
 * block, which may walk the cache. The last printed line is what makes the
 * timings legible: how many calls the translated-word test answered on its own,
 * and how many blocks a walk examined.
 *
 * The gate is deliberately loose. It exists to catch a per-store walk over every
 * block, not to measure a machine, so its bound is three orders of magnitude
 * above the value it measures.
 */

#include "dynarec_fixture.h"

#include <inttypes.h>
#include <stdio.h>
#include <time.h>

#define PROBE_BLOCKS 2048u
#define PROBE_STRIDE 8u
#define PROBE_ITERATIONS 200000u

/* A word past the end of the translated sweep, and the first word of it. */
#define PROBE_MISS_ADDR 0x100000u
#define PROBE_HIT_ADDR 0u

/* What a guard test may cost. The measured value is far below this. */
#define MISS_BUDGET_NS 2000u

static uint64_t monotonic_ns(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000000000u + (uint64_t)now.tv_nsec;
}

static void install_sweep(struct fixture *fixture)
{
	unsigned int i, j, w;

	for (i = 0; i < PROBE_BLOCKS; i++) {
		w = i * PROBE_STRIDE;
		for (j = 0; j < PROBE_STRIDE - 2; j++)
			fixture->ram[w + j] = 0x24420001; /* addiu $v0, $v0, 1 */
		fixture->ram[w + PROBE_STRIDE - 2] = 0x08000000 | (w + PROBE_STRIDE);
		fixture->ram[w + PROBE_STRIDE - 1] = 0x24420001; /* delay slot */
	}
	fixture->ram[PROBE_BLOCKS * PROBE_STRIDE] = 0x0000000c; /* syscall */
}

static uint64_t measure_ns_per_call(struct fixture *fixture, uint32_t addr, uint32_t len)
{
	const struct lightrec_execution_stats *stats = lightrec_get_execution_stats(fixture->state);
	uint64_t before, start, elapsed;
	unsigned int i;

	/* Warm the call path before the timed loop. */
	for (i = 0; i < 1000; i++)
		lightrec_invalidate(fixture->state, addr, len);

	before = stats->invalidations;
	start = monotonic_ns();
	for (i = 0; i < PROBE_ITERATIONS; i++)
		lightrec_invalidate(fixture->state, addr, len);
	elapsed = monotonic_ns() - start;

	/* The control for the timing: the loop made exactly the calls it claims,
	 * so a number below is a cost per call and not a cost per nothing. */
	if (stats->invalidations != before + PROBE_ITERATIONS) {
		fprintf(stderr, "the timed loop made %" PRIu64 " calls, not %u\n",
			stats->invalidations - before, PROBE_ITERATIONS);
		return UINT64_MAX;
	}

	return elapsed / PROBE_ITERATIONS;
}

int main(void)
{
	const struct lightrec_execution_stats *stats;
	struct fixture fixture;
	uint64_t miss_ns, hit_ns, empty_ns;
	int failed = 0;

	if (fixture_init(&fixture, "lightrec_invalidation_cost_probe"))
		return 1;

	install_sweep(&fixture);
	lightrec_execute(fixture.state, 0, 0x7fffffff);
	stats = lightrec_get_execution_stats(fixture.state);
	printf("translated_blocks=%" PRIu64 " translated_instructions=%" PRIu64
	       " fallback_blocks=%" PRIu64 "\n",
	       stats->translated_blocks, stats->translated_instructions, stats->fallback_blocks);
	if (stats->translated_blocks < PROBE_BLOCKS) {
		fputs("cost probe: the sweep did not translate enough blocks to measure\n", stderr);
		fixture_destroy(&fixture);
		return 1;
	}

	miss_ns = measure_ns_per_call(&fixture, PROBE_MISS_ADDR, 4);
	hit_ns = measure_ns_per_call(&fixture, PROBE_HIT_ADDR, 4);
	empty_ns = measure_ns_per_call(&fixture, PROBE_HIT_ADDR, 0);

	stats = lightrec_get_execution_stats(fixture.state);
	printf("per call: untranslated word %" PRIu64 " ns, translated word %" PRIu64
	       " ns, empty range %" PRIu64 " ns\n",
	       miss_ns, hit_ns, empty_ns);
	printf("cumulative: invalidations=%" PRIu64 " words=%" PRIu64 " guards=%" PRIu64
	       " scans=%" PRIu64 " revoked=%" PRIu64 "\n",
	       stats->invalidations, stats->invalidation_words, stats->invalidation_guards,
	       stats->invalidation_scans, stats->invalidated_blocks);

	if (miss_ns == UINT64_MAX || empty_ns == UINT64_MAX) {
		fputs("cost probe: the counters did not advance, so nothing was measured\n",
		      stderr);
		fixture_destroy(&fixture);
		return 1;
	}

	if (miss_ns > MISS_BUDGET_NS) {
		fprintf(stderr,
			"a store into an untranslated word cost %" PRIu64
			" ns, over the %u ns budget for a guard test\n",
			miss_ns, MISS_BUDGET_NS);
		failed = 1;
	}
	if (empty_ns > MISS_BUDGET_NS) {
		fprintf(stderr, "an empty ranged invalidation cost %" PRIu64 " ns\n", empty_ns);
		failed = 1;
	}

	fixture_destroy(&fixture);
	return failed;
}
