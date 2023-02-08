/* Spa */
/* SPDX-FileCopyrightText: Copyright © 2020 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <assert.h>

#include <spa/utils/dict.h>
#include <spa/utils/string.h>

#define MAX_COUNT 100000
#define MAX_ITEMS 1000

static struct spa_dict_item items[MAX_ITEMS];
static char values[MAX_ITEMS][32];

static void gen_values(void)
{
	uint32_t i, j, idx;
	static const char chars[] = "abcdefghijklmnopqrstuvwxyz.:*ABCDEFGHIJKLMNOPQRSTUVWXYZ";

	for (i = 0; i < MAX_ITEMS; i++) {
		for (j = 0; j < 32; j++) {
			idx = random() % sizeof(chars);
			values[i][j] = chars[idx];
		}
		idx = random() % 16;
		values[i][idx + 16] = 0;
	}
}

static void gen_dict(struct spa_dict *dict, uint32_t n_items)
{
	uint32_t i, idx;

	for (i = 0; i < n_items; i++) {
		idx = random() % MAX_ITEMS;
		items[i] = SPA_DICT_ITEM_INIT(values[idx], values[idx]);
	}
	dict->items = items;
	dict->n_items = n_items;
	dict->flags = 0;
}

static void test_query(const struct spa_dict *dict)
{
	uint32_t i, idx;
	const char *str;

	for (i = 0; i < MAX_COUNT; i++) {
		idx = random() % dict->n_items;
		str = spa_dict_lookup(dict, dict->items[idx].key);
		assert(spa_streq(str, dict->items[idx].value));
	}
}

static void test_lookup(struct spa_dict *dict)
{
	struct timespec ts;
	uint64_t t1, t2, t3, t4;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	t1 = SPA_TIMESPEC_TO_NSEC(&ts);

	test_query(dict);

	clock_gettime(CLOCK_MONOTONIC, &ts);
	t2 = SPA_TIMESPEC_TO_NSEC(&ts);

	fprintf(stderr, "%d elapsed %"PRIu64" count %u = %"PRIu64"/sec\n", dict->n_items,
			t2 - t1, MAX_COUNT, MAX_COUNT * (uint64_t)SPA_NSEC_PER_SEC / (t2 - t1));

	spa_dict_qsort(dict);

	clock_gettime(CLOCK_MONOTONIC, &ts);
	t3 = SPA_TIMESPEC_TO_NSEC(&ts);

	fprintf(stderr, "%d sort elapsed %"PRIu64"\n", dict->n_items, t3 - t2);

	clock_gettime(CLOCK_MONOTONIC, &ts);
	t3 = SPA_TIMESPEC_TO_NSEC(&ts);

	test_query(dict);

	clock_gettime(CLOCK_MONOTONIC, &ts);
	t4 = SPA_TIMESPEC_TO_NSEC(&ts);

	fprintf(stderr, "%d elapsed %"PRIu64" count %u = %"PRIu64"/sec %f speedup\n", dict->n_items,
			t4 - t3, MAX_COUNT, MAX_COUNT * (uint64_t)SPA_NSEC_PER_SEC / (t4 - t3),
			(double)(t2 - t1) / (t4 - t2));
}

int main(int argc, char *argv[])
{
	struct spa_dict dict;

	spa_zero(dict);
	gen_values();

	/* warmup */
	gen_dict(&dict, 1000);
	test_query(&dict);

	gen_dict(&dict, 10);
	test_lookup(&dict);

	gen_dict(&dict, 20);
	test_lookup(&dict);

	gen_dict(&dict, 50);
	test_lookup(&dict);

	gen_dict(&dict, 100);
	test_lookup(&dict);

	gen_dict(&dict, 1000);
	test_lookup(&dict);

	return 0;
}
