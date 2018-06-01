/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering

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

#include <sys/types.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>

#include <pulse/xmalloc.h>

#include "internal.h"
#include "strbuf.h"

/* A chunk of the linked list that makes up the string */
struct chunk {
    struct chunk *next;
    size_t length;
};

#define CHUNK_TO_TEXT(c) ((char*) (c) + PA_ALIGN(sizeof(struct chunk)))

struct pa_strbuf {
    size_t length;
    struct chunk *head, *tail;
};

pa_strbuf *pa_strbuf_new(void) {
    pa_strbuf *sb;

    sb = pa_xnew(pa_strbuf, 1);
    sb->length = 0;
    sb->head = sb->tail = NULL;

    return sb;
}

void pa_strbuf_free(pa_strbuf *sb) {
    pa_assert(sb);

    while (sb->head) {
        struct chunk *c = sb->head;
        sb->head = sb->head->next;
        pa_xfree(c);
    }

    pa_xfree(sb);
}

/* Make a C string from the string buffer. The caller has to free
 * string with pa_xfree(). */
char *pa_strbuf_to_string(pa_strbuf *sb) {
    char *t, *e;
    struct chunk *c;

    pa_assert(sb);

    e = t = pa_xmalloc(sb->length+1);

    for (c = sb->head; c; c = c->next) {
        pa_assert((size_t) (e-t) <= sb->length);
        memcpy(e, CHUNK_TO_TEXT(c), c->length);
        e += c->length;
    }

    /* Trailing NUL */
    *e = 0;

    pa_assert(e == t+sb->length);

    return t;
}

/* Combination of pa_strbuf_free() and pa_strbuf_to_string() */
char *pa_strbuf_to_string_free(pa_strbuf *sb) {
    char *t;

    pa_assert(sb);
    t = pa_strbuf_to_string(sb);
    pa_strbuf_free(sb);

    return t;
}

/* Append a string to the string buffer */
void pa_strbuf_puts(pa_strbuf *sb, const char *t) {

    pa_assert(sb);
    pa_assert(t);

    pa_strbuf_putsn(sb, t, strlen(t));
}

/* Append a character to the string buffer */
void pa_strbuf_putc(pa_strbuf *sb, char c) {
    pa_assert(sb);

    pa_strbuf_putsn(sb, &c, 1);
}

/* Append a new chunk to the linked list */
static void append(pa_strbuf *sb, struct chunk *c) {
    pa_assert(sb);
    pa_assert(c);

    if (sb->tail) {
        pa_assert(sb->head);
        sb->tail->next = c;
    } else {
        pa_assert(!sb->head);
        sb->head = c;
    }

    sb->tail = c;
    sb->length += c->length;
    c->next = NULL;
}

/* Append up to l bytes of a string to the string buffer */
void pa_strbuf_putsn(pa_strbuf *sb, const char *t, size_t l) {
    struct chunk *c;

    pa_assert(sb);
    pa_assert(t);

    if (!l)
        return;

    c = pa_xmalloc(PA_ALIGN(sizeof(struct chunk)) + l);
    c->length = l;
    memcpy(CHUNK_TO_TEXT(c), t, l);

    append(sb, c);
}

/* Append a printf() style formatted string to the string buffer. */
/* The following is based on an example from the GNU libc documentation */
size_t pa_strbuf_printf(pa_strbuf *sb, const char *format, ...) {
    size_t size = 100;
    struct chunk *c = NULL;

    pa_assert(sb);
    pa_assert(format);

    for(;;) {
        va_list ap;
        int r;

        c = pa_xrealloc(c, PA_ALIGN(sizeof(struct chunk)) + size);

        va_start(ap, format);
        r = vsnprintf(CHUNK_TO_TEXT(c), size, format, ap);
        CHUNK_TO_TEXT(c)[size-1] = 0;
        va_end(ap);

        if (r > -1 && (size_t) r < size) {
            c->length = (size_t) r;
            append(sb, c);
            return (size_t) r;
        }

        if (r > -1)    /* glibc 2.1 */
            size = (size_t) r+1;
        else           /* glibc 2.0 */
            size *= 2;
    }
}

bool pa_strbuf_isempty(pa_strbuf *sb) {
    pa_assert(sb);

    return sb->length <= 0;
}
