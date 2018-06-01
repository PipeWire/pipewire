#ifndef foopulsecorehashmaphfoo
#define foopulsecorehashmaphfoo

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

#include <spa/utils/defs.h>

#include <pulse/def.h>

typedef unsigned (*pa_hash_func_t)(const void *p);
typedef int (*pa_compare_func_t)(const void *a, const void *b);
typedef void *(*pa_copy_func_t)(const void *p);

/* Simple Implementation of a hash table. Memory management is the
 * user's job. It's a good idea to have the key pointer point to a
 * string in the value data. The insertion order is preserved when
 * iterating. */

typedef struct pa_hashmap pa_hashmap;

/* Create a new hashmap. Use the specified functions for hashing and comparing objects in the map */
pa_hashmap *pa_hashmap_new(pa_hash_func_t hash_func, pa_compare_func_t compare_func);

/* Create a new hashmap. Use the specified functions for hashing and comparing objects in the map, and functions to free the key
 * and value (either or both can be NULL). */
pa_hashmap *pa_hashmap_new_full(pa_hash_func_t hash_func, pa_compare_func_t compare_func, pa_free_cb_t key_free_func, pa_free_cb_t value_free_func);

/* Free the hash table. */
void pa_hashmap_free(pa_hashmap*);

/* Add an entry to the hashmap. Returns non-zero when the entry already exists */
int pa_hashmap_put(pa_hashmap *h, void *key, void *value);

/* Return an entry from the hashmap */
void* pa_hashmap_get(pa_hashmap *h, const void *key);

/* Returns the data of the entry while removing */
void* pa_hashmap_remove(pa_hashmap *h, const void *key);

/* Removes the entry and frees the entry data. Returns a negative value if the
 * entry is not found. FIXME: This function shouldn't be needed.
 * pa_hashmap_remove() should free the entry data, and the current semantics of
 * pa_hashmap_remove() should be implemented by a function called
 * pa_hashmap_steal(). */
int pa_hashmap_remove_and_free(pa_hashmap *h, const void *key);

/* Remove all entries but don't free the hashmap */
void pa_hashmap_remove_all(pa_hashmap *h);

/* Return the current number of entries of the hashmap */
unsigned pa_hashmap_size(pa_hashmap *h);

/* Return true if the hashmap is empty */
bool pa_hashmap_isempty(pa_hashmap *h);

/* May be used to iterate through the hashmap. Initially the opaque
   pointer *state has to be set to NULL. The hashmap may not be
   modified during iteration -- except for deleting the current entry
   via pa_hashmap_remove(). The key of the entry is returned in *key,
   if key is non-NULL. After the last entry in the hashmap NULL is
   returned. */
void *pa_hashmap_iterate(pa_hashmap *h, void **state, const void**key);

/* Same as pa_hashmap_iterate() but goes backwards */
void *pa_hashmap_iterate_backwards(pa_hashmap *h, void **state, const void**key);

/* Remove the oldest entry in the hashmap and return it */
void *pa_hashmap_steal_first(pa_hashmap *h);

/* Return the oldest entry in the hashmap */
void* pa_hashmap_first(pa_hashmap *h);

/* Return the newest entry in the hashmap */
void* pa_hashmap_last(pa_hashmap *h);

/* A macro to ease iteration through all entries */
#define PA_HASHMAP_FOREACH(e, h, state) \
    for ((state) = NULL, (e) = pa_hashmap_iterate((h), &(state), NULL); (e); (e) = pa_hashmap_iterate((h), &(state), NULL))

/* A macro to ease itration through all key, value pairs */
#define PA_HASHMAP_FOREACH_KV(k, e, h, state) \
    for ((state) = NULL, (e) = pa_hashmap_iterate((h), &(state), (const void **) &(k)); (e); (e) = pa_hashmap_iterate((h), &(state), (const void **) &(k)))

/* A macro to ease iteration through all entries, backwards */
#define PA_HASHMAP_FOREACH_BACKWARDS(e, h, state) \
    for ((state) = NULL, (e) = pa_hashmap_iterate_backwards((h), &(state), NULL); (e); (e) = pa_hashmap_iterate_backwards((h), &(state), NULL))

#endif
