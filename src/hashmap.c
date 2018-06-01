/***
  This file is part of PulseAudio.

  Copyright 2004-2008 Lennart Poettering

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2.1 of the License,
  or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with PulseAudio; if not, see <http://www.gnu.org/licenses/>.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdlib.h>

#include <pulse/xmalloc.h>

#include "internal.h"
#include "hashmap.h"

#define NBUCKETS 127

struct hashmap_entry {
    void *key;
    void *value;

    struct hashmap_entry *bucket_next, *bucket_previous;
    struct hashmap_entry *iterate_next, *iterate_previous;
};

struct pa_hashmap {
    pa_hash_func_t hash_func;
    pa_compare_func_t compare_func;

    pa_free_cb_t key_free_func;
    pa_free_cb_t value_free_func;

    struct hashmap_entry *iterate_list_head, *iterate_list_tail;
    unsigned n_entries;
};

#define BY_HASH(h) ((struct hashmap_entry**) ((uint8_t*) (h) + PA_ALIGN(sizeof(pa_hashmap))))

PA_STATIC_FLIST_DECLARE(entries, 0, pa_xfree);

static unsigned pa_idxset_trivial_hash_func(const void *p) {
    return PA_PTR_TO_UINT(p);
}

static int pa_idxset_trivial_compare_func(const void *a, const void *b) {
    return a < b ? -1 : (a > b ? 1 : 0);
}

pa_hashmap *pa_hashmap_new_full(pa_hash_func_t hash_func, pa_compare_func_t compare_func, pa_free_cb_t key_free_func, pa_free_cb_t value_free_func) {
    pa_hashmap *h;

    h = pa_xmalloc0(PA_ALIGN(sizeof(pa_hashmap)) + NBUCKETS*sizeof(struct hashmap_entry*));

    h->hash_func = hash_func ? hash_func : pa_idxset_trivial_hash_func;
    h->compare_func = compare_func ? compare_func : pa_idxset_trivial_compare_func;

    h->key_free_func = key_free_func;
    h->value_free_func = value_free_func;

    h->n_entries = 0;
    h->iterate_list_head = h->iterate_list_tail = NULL;

    return h;
}

pa_hashmap *pa_hashmap_new(pa_hash_func_t hash_func, pa_compare_func_t compare_func) {
    return pa_hashmap_new_full(hash_func, compare_func, NULL, NULL);
}

static void remove_entry(pa_hashmap *h, struct hashmap_entry *e) {
    pa_assert(h);
    pa_assert(e);

    /* Remove from iteration list */
    if (e->iterate_next)
        e->iterate_next->iterate_previous = e->iterate_previous;
    else
        h->iterate_list_tail = e->iterate_previous;

    if (e->iterate_previous)
        e->iterate_previous->iterate_next = e->iterate_next;
    else
        h->iterate_list_head = e->iterate_next;

    /* Remove from hash table bucket list */
    if (e->bucket_next)
        e->bucket_next->bucket_previous = e->bucket_previous;

    if (e->bucket_previous)
        e->bucket_previous->bucket_next = e->bucket_next;
    else {
        unsigned hash = h->hash_func(e->key) % NBUCKETS;
        BY_HASH(h)[hash] = e->bucket_next;
    }

    if (h->key_free_func)
        h->key_free_func(e->key);

    if (pa_flist_push(PA_STATIC_FLIST_GET(entries), e) < 0)
        pa_xfree(e);

    pa_assert(h->n_entries >= 1);
    h->n_entries--;
}

void pa_hashmap_free(pa_hashmap *h) {
    pa_assert(h);

    pa_hashmap_remove_all(h);
    pa_xfree(h);
}

static struct hashmap_entry *hash_scan(pa_hashmap *h, unsigned hash, const void *key) {
    struct hashmap_entry *e;
    pa_assert(h);
    pa_assert(hash < NBUCKETS);

    for (e = BY_HASH(h)[hash]; e; e = e->bucket_next)
        if (h->compare_func(e->key, key) == 0)
            return e;

    return NULL;
}

int pa_hashmap_put(pa_hashmap *h, void *key, void *value) {
    struct hashmap_entry *e;
    unsigned hash;

    pa_assert(h);

    hash = h->hash_func(key) % NBUCKETS;

    if (hash_scan(h, hash, key))
        return -1;

    if (!(e = pa_flist_pop(PA_STATIC_FLIST_GET(entries))))
        e = pa_xnew(struct hashmap_entry, 1);

    e->key = key;
    e->value = value;

    /* Insert into hash table */
    e->bucket_next = BY_HASH(h)[hash];
    e->bucket_previous = NULL;
    if (BY_HASH(h)[hash])
        BY_HASH(h)[hash]->bucket_previous = e;
    BY_HASH(h)[hash] = e;

    /* Insert into iteration list */
    e->iterate_previous = h->iterate_list_tail;
    e->iterate_next = NULL;
    if (h->iterate_list_tail) {
        pa_assert(h->iterate_list_head);
        h->iterate_list_tail->iterate_next = e;
    } else {
        pa_assert(!h->iterate_list_head);
        h->iterate_list_head = e;
    }
    h->iterate_list_tail = e;

    h->n_entries++;
    pa_assert(h->n_entries >= 1);

    return 0;
}

void* pa_hashmap_get(pa_hashmap *h, const void *key) {
    unsigned hash;
    struct hashmap_entry *e;

    pa_assert(h);

    hash = h->hash_func(key) % NBUCKETS;

    if (!(e = hash_scan(h, hash, key)))
        return NULL;

    return e->value;
}

void* pa_hashmap_remove(pa_hashmap *h, const void *key) {
    struct hashmap_entry *e;
    unsigned hash;
    void *data;

    pa_assert(h);

    hash = h->hash_func(key) % NBUCKETS;

    if (!(e = hash_scan(h, hash, key)))
        return NULL;

    data = e->value;
    remove_entry(h, e);

    return data;
}

int pa_hashmap_remove_and_free(pa_hashmap *h, const void *key) {
    void *data;

    pa_assert(h);

    data = pa_hashmap_remove(h, key);

    if (data && h->value_free_func)
        h->value_free_func(data);

    return data ? 0 : -1;
}

void pa_hashmap_remove_all(pa_hashmap *h) {
    pa_assert(h);

    while (h->iterate_list_head) {
        void *data;
        data = h->iterate_list_head->value;
        remove_entry(h, h->iterate_list_head);

        if (h->value_free_func)
            h->value_free_func(data);
    }
}

void *pa_hashmap_iterate(pa_hashmap *h, void **state, const void **key) {
    struct hashmap_entry *e;

    pa_assert(h);
    pa_assert(state);

    if (*state == (void*) -1)
        goto at_end;

    if (!*state && !h->iterate_list_head)
        goto at_end;

    e = *state ? *state : h->iterate_list_head;

    if (e->iterate_next)
        *state = e->iterate_next;
    else
        *state = (void*) -1;

    if (key)
        *key = e->key;

    return e->value;

at_end:
    *state = (void *) -1;

    if (key)
        *key = NULL;

    return NULL;
}

void *pa_hashmap_iterate_backwards(pa_hashmap *h, void **state, const void **key) {
    struct hashmap_entry *e;

    pa_assert(h);
    pa_assert(state);

    if (*state == (void*) -1)
        goto at_beginning;

    if (!*state && !h->iterate_list_tail)
        goto at_beginning;

    e = *state ? *state : h->iterate_list_tail;

    if (e->iterate_previous)
        *state = e->iterate_previous;
    else
        *state = (void*) -1;

    if (key)
        *key = e->key;

    return e->value;

at_beginning:
    *state = (void *) -1;

    if (key)
        *key = NULL;

    return NULL;
}

void* pa_hashmap_first(pa_hashmap *h) {
    pa_assert(h);

    if (!h->iterate_list_head)
        return NULL;

    return h->iterate_list_head->value;
}

void* pa_hashmap_last(pa_hashmap *h) {
    pa_assert(h);

    if (!h->iterate_list_tail)
        return NULL;

    return h->iterate_list_tail->value;
}

void* pa_hashmap_steal_first(pa_hashmap *h) {
    void *data;

    pa_assert(h);

    if (!h->iterate_list_head)
        return NULL;

    data = h->iterate_list_head->value;
    remove_entry(h, h->iterate_list_head);

    return data;
}

unsigned pa_hashmap_size(pa_hashmap *h) {
    pa_assert(h);

    return h->n_entries;
}

bool pa_hashmap_isempty(pa_hashmap *h) {
    pa_assert(h);

    return h->n_entries == 0;
}
