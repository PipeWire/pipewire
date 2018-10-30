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
#include <spa/utils/list.h>

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

struct string_list {
    char string[20];
    struct spa_list node;
};

static void test_list(void)
{
    struct string_list list;
    struct spa_list *head = &list.node;
    struct string_list *e;
    int i;

    spa_list_init(head);
    spa_assert(spa_list_is_empty(head));

    e = malloc(sizeof(struct string_list));
    strcpy(e->string, "test");
    spa_list_insert(head, &e->node);
    spa_assert(!spa_list_is_empty(head));
    spa_assert(spa_list_first(head, struct string_list, node) == e);
    spa_assert(spa_list_last(head, struct string_list, node) == e);

    e = malloc(sizeof(struct string_list));
    strcpy(e->string, "pipewire!");
    spa_list_append(head, &e->node);
    spa_assert(!spa_list_is_empty(head));
    spa_assert(spa_list_last(head, struct string_list, node) == e);

    e = malloc(sizeof(struct string_list));
    strcpy(e->string, "First element");
    spa_list_prepend(head, &e->node);
    spa_assert(!spa_list_is_empty(head));
    spa_assert(spa_list_first(head, struct string_list, node) == e);

    i = 0;
    spa_list_for_each(e, head, node) {
        switch (i++) {
        case 0:
            spa_assert(!strcmp(e->string, "First element"));
            break;
        case 1:
            spa_assert(!strcmp(e->string, "test"));
            break;
        case 2:
            spa_assert(!strcmp(e->string, "pipewire!"));
            break;
        default:
            spa_assert_not_reached();
            break;
        }
    }

    i = 0;
    spa_list_consume(e, head, node) {
        spa_list_remove(&e->node);
        free(e);
        i++;
    }
    spa_assert(i == 3);
    spa_assert(spa_list_is_empty(head));
}

int main(int argc, char *argv[])
{
    test_dict();
    test_list();
    return 0;
}
