/* PipeWire
 * Copyright (C) 2019 Wim Taymans <wim.taymans@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include <pipewire/array.h>

static void test_abi(void)
{
	/* array */
	spa_assert(sizeof(struct pw_array) == 32);
}

static void test_array(void)
{
	struct pw_array arr;
	uint32_t *ptr;
	uint32_t vals[] = { 0, 100, 0x8a, 0 };
	size_t i;

	pw_array_init(&arr, 64);
	spa_assert(SPA_N_ELEMENTS(vals) == 4);

	spa_assert(pw_array_get_len(&arr, uint32_t) == 0);
	spa_assert(pw_array_check_index(&arr, 0, uint32_t) == false);
	spa_assert(pw_array_first(&arr) == pw_array_end(&arr));
	pw_array_for_each(ptr, &arr)
		spa_assert_not_reached();

	for (i = 0; i < 4; i++) {
		ptr = (uint32_t*)pw_array_add(&arr, sizeof(uint32_t));
		*ptr = vals[i];
	}

	spa_assert(pw_array_get_len(&arr, uint32_t) == 4);
	spa_assert(pw_array_check_index(&arr, 2, uint32_t) == true);
	spa_assert(pw_array_check_index(&arr, 3, uint32_t) == true);
	spa_assert(pw_array_check_index(&arr, 4, uint32_t) == false);

	i = 0;
	pw_array_for_each(ptr, &arr) {
		spa_assert(*ptr == vals[i++]);
	}

	/* remove second */
	ptr = pw_array_get_unchecked(&arr, 2, uint32_t);
	spa_assert(ptr != NULL);
	pw_array_remove(&arr, ptr);
	spa_assert(pw_array_get_len(&arr, uint32_t) == 3);
	spa_assert(pw_array_check_index(&arr, 3, uint32_t) == false);
	ptr = pw_array_get_unchecked(&arr, 2, uint32_t);
	spa_assert(ptr != NULL);
	spa_assert(*ptr == vals[3]);

	/* remove first */
	ptr = pw_array_get_unchecked(&arr, 0, uint32_t);
	spa_assert(ptr != NULL);
	pw_array_remove(&arr, ptr);
	spa_assert(pw_array_get_len(&arr, uint32_t) == 2);
	ptr = pw_array_get_unchecked(&arr, 0, uint32_t);
	spa_assert(ptr != NULL);
	spa_assert(*ptr == vals[1]);

	/* iterate */
	ptr = (uint32_t*)pw_array_first(&arr);
	spa_assert(pw_array_check(&arr, ptr));
	spa_assert(*ptr == vals[1]);
	ptr++;
	spa_assert(pw_array_check(&arr, ptr));
	spa_assert(*ptr == vals[3]);
	ptr++;
	spa_assert(pw_array_check(&arr, ptr) == false);

	pw_array_reset(&arr);
	spa_assert(pw_array_get_len(&arr, uint32_t) == 0);

	pw_array_clear(&arr);
}

int main(int argc, char *argv[])
{
	test_abi();
	test_array();

	return 0;
}
