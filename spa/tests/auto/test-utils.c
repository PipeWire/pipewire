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
#include <spa/utils/hook.h>
#include <spa/utils/ringbuffer.h>

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


struct my_hook {
    int version;
    void (*invoke) (void *);
};

struct my_hook_data {
    bool cb1;
    bool cb2;
    bool cb3;
};

static void test_hook_callback_1(void *data)
{
    ((struct my_hook_data *) data)->cb1 = true;
}

static void test_hook_callback_2(void *data)
{
    ((struct my_hook_data *) data)->cb2 = true;
}

static void test_hook_callback_3(void *data)
{
    ((struct my_hook_data *) data)->cb3 = true;
}

static void test_hook_callback_4(void *data)
{
    spa_assert_not_reached();
}

static int hook_free_count = 0;

static void hook_removed_cb(struct spa_hook *h)
{
    free(h);
    hook_free_count++;
}

static void test_hook(void)
{
    const int VERSION = 2;
    struct spa_hook_list hl;
    struct my_hook callbacks[4] = {
        {2, test_hook_callback_1},
        {3, test_hook_callback_2},
        {2, test_hook_callback_3},
        /* version 1 should not be called */
        {1, test_hook_callback_4}
    };
    struct my_hook_data data = {0};
    struct spa_hook *h;
    int count = 0;

    spa_hook_list_init(&hl);

    h = malloc(sizeof(struct spa_hook));
    h->removed = hook_removed_cb;
    spa_hook_list_append(&hl, h, &callbacks[1], &data);

    h = malloc(sizeof(struct spa_hook));
    h->removed = hook_removed_cb;
    spa_hook_list_append(&hl, h, &callbacks[2], &data);

    /* iterate with the simple API */
    spa_hook_list_call_simple(&hl, struct my_hook, invoke, VERSION);
    spa_assert(data.cb1 == false);
    spa_assert(data.cb2 == true);
    spa_assert(data.cb3 == true);

    /* reset cb* booleans to false */
    memset(&data, 0, sizeof(struct my_hook_data));

    h = malloc(sizeof(struct spa_hook));
    h->removed = hook_removed_cb;
    spa_hook_list_prepend(&hl, h, &callbacks[0], &data);

    /* call only the first hook - this should be callback_1 */
    count = spa_hook_list_call_once(&hl, struct my_hook, invoke, VERSION);
    spa_assert(count == 1);
    spa_assert(data.cb1 == true);
    spa_assert(data.cb2 == false);
    spa_assert(data.cb3 == false);

    /* reset cb* booleans to false */
    memset(&data, 0, sizeof(struct my_hook_data));

    /* add callback_4 - this is version 1, so it shouldn't be executed */
    h = malloc(sizeof(struct spa_hook));
    h->removed = hook_removed_cb;
    spa_hook_list_append(&hl, h, &callbacks[3], &data);

    count = spa_hook_list_call(&hl, struct my_hook, invoke, VERSION);
    spa_assert(count == 3);
    spa_assert(data.cb1 == true);
    spa_assert(data.cb2 == true);
    spa_assert(data.cb3 == true);

    count = 0;
    hook_free_count = 0;
    spa_list_consume(h, &hl.list, link) {
        spa_hook_remove(h);
        count++;
    }
    spa_assert(count == 4);
    spa_assert(hook_free_count == 4);
}

static void test_ringbuffer(void)
{
    struct spa_ringbuffer rb;
    char buffer[20];
    char readbuf[20];
    uint32_t idx;
    int32_t fill;

    spa_ringbuffer_init(&rb);
    fill = spa_ringbuffer_get_write_index(&rb, &idx);
    spa_assert(idx == 0);
    spa_assert(fill == 0);

    spa_ringbuffer_write_data(&rb, buffer, 20, idx, "hello pipewire", 14);
    spa_ringbuffer_write_update(&rb, idx + 14);

    fill = spa_ringbuffer_get_write_index(&rb, &idx);
    spa_assert(idx == 14);
    spa_assert(fill == 14);
    fill = spa_ringbuffer_get_read_index(&rb, &idx);
    spa_assert(idx == 0);
    spa_assert(fill == 14);

    spa_ringbuffer_read_data(&rb, buffer, 20, idx, readbuf, 6);
    spa_ringbuffer_read_update(&rb, idx + 6);
    spa_assert(!memcmp(readbuf, "hello ", 6));

    fill = spa_ringbuffer_get_read_index(&rb, &idx);
    spa_assert(idx == 6);
    spa_assert(fill == 8);
    fill = spa_ringbuffer_get_write_index(&rb, &idx);
    spa_assert(idx == 14);
    spa_assert(fill == 8);

    spa_ringbuffer_write_data(&rb, buffer, 20, idx, " rocks !!!", 10);
    spa_ringbuffer_write_update(&rb, idx + 10);

    fill = spa_ringbuffer_get_write_index(&rb, &idx);
    spa_assert(idx == 24);
    spa_assert(fill == 18);
    fill = spa_ringbuffer_get_read_index(&rb, &idx);
    spa_assert(idx == 6);
    spa_assert(fill == 18);

    spa_ringbuffer_read_data(&rb, buffer, 20, idx, readbuf, 18);
    spa_ringbuffer_read_update(&rb, idx + 18);
    spa_assert(!memcmp(readbuf, "pipewire rocks !!!", 18));

    fill = spa_ringbuffer_get_read_index(&rb, &idx);
    spa_assert(idx == 24);
    spa_assert(fill == 0);
    fill = spa_ringbuffer_get_write_index(&rb, &idx);
    spa_assert(idx == 24);
    spa_assert(fill == 0);

    /* actual buffer must have wrapped around */
    spa_assert(!memcmp(buffer, " !!!o pipewire rocks", 20));
}

int main(int argc, char *argv[])
{
    test_dict();
    test_list();
    test_hook();
    test_ringbuffer();
    return 0;
}
