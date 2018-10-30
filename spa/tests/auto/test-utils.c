/* Simple Plugin API
 * Copyright (C) 2018 Collabora Ltd.
 *   @author George Kiagiadakis <george.kiagiadakis@collabora.com>
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

#include <spa/utils/dict.h>

static void test_dict(void)
{
    struct spa_dict_item items[5] = {
        SPA_DICT_ITEM_INIT("key", "value"),
        SPA_DICT_ITEM_INIT("pipe", "wire"),
        SPA_DICT_ITEM_INIT("test", "Works!"),
        SPA_DICT_ITEM_INIT("123", ""),
        SPA_DICT_ITEM_INIT("SPA", "Simple Plugin API"),
    };
    struct spa_dict dict = SPA_DICT_INIT_ARRAY (items);

    spa_assert(dict.n_items == 5);
    spa_assert(!strcmp(spa_dict_lookup(&dict, "pipe"), "wire"));
    spa_assert(!strcmp(spa_dict_lookup(&dict, "123"), ""));
    spa_assert(!strcmp(spa_dict_lookup(&dict, "key"), "value"));
    spa_assert(!strcmp(spa_dict_lookup(&dict, "SPA"), "Simple Plugin API"));
    spa_assert(!strcmp(spa_dict_lookup(&dict, "test"), "Works!"));
    spa_assert(spa_dict_lookup(&dict, "nonexistent") == NULL);
}

int main(int argc, char *argv[])
{
    test_dict();
    return 0;
}
