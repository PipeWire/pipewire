/***
  This file is part of PulseAudio.

  Copyright 2009 Lennart Poettering

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

#include <string.h>
#include <stdbool.h>

#include "internal.h"
#include "bitset.h"

void pa_bitset_set(pa_bitset_t *b, unsigned k, bool v) {
    pa_assert(b);

    if (v)
        b[k >> 5] |= 1 << (k & 31);
    else
        b[k >> 5] &= ~((uint32_t) (1 << (k & 31)));
}

bool pa_bitset_get(const pa_bitset_t *b, unsigned k) {
    return !!(b[k >> 5] & (1 << (k & 31)));
}

bool pa_bitset_equals(const pa_bitset_t *b, unsigned n, ...) {
    va_list ap;
    pa_bitset_t *a;
    bool equal;

    a = alloca(PA_BITSET_ELEMENTS(n));

    va_start(ap, n);
    for (;;) {
        int j = va_arg(ap, int);

        if (j < 0)
            break;

        pa_bitset_set(a, j, true);
    }
    va_end(ap);

    equal = memcmp(a, b, PA_BITSET_SIZE(n)) == 0;

    return equal;
}
