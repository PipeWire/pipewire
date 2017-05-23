/* PipeWire
 * Copyright (C) 2016 Wim Taymans <wim.taymans@gmail.com>
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

#include <string.h>

#include <pipewire/client/array.h>
#include <pipewire/client/log.h>
#include <pipewire/client/utils.h>

const char *
pw_split_walk (const char  *str,
               const char  *delimiter,
               size_t      *len,
               const char **state)
{
  const char *s = *state ? *state : str;

  if (*s == '\0')
    return NULL;

  *len = strcspn (s, delimiter);

  *state = s + *len;
  *state += strspn (*state, delimiter);

  return s;
}

char **
pw_split_strv (const char *str,
               const char *delimiter,
               int         max_tokens,
               int        *n_tokens)
{
  const char *state = NULL, *s = NULL;
  struct pw_array arr;
  size_t len;
  int n = 0;

  pw_array_init (&arr, 16);

  s = pw_split_walk (str, delimiter, &len, &state);
  while (s && n + 1 < max_tokens) {
    pw_array_add_ptr (&arr, strndup (s, len));
    s = pw_split_walk (str, delimiter, &len, &state);
    n++;
  }
  if (s) {
    pw_array_add_ptr (&arr, strdup (s));
    n++;
  }
  pw_array_add_ptr (&arr, NULL);

  *n_tokens = n;

  return arr.data;
}


void
pw_free_strv (char **str)
{
  int i;
  for (i = 0; str[i]; i++)
    free (str[i]);
  free (str);
}

char *
pw_strip (char       *str,
          const char *whitespace)
{
  char *e, *l = NULL;

  str += strspn (str, whitespace);

  for (e = str; *e; e++)
     if (!strchr (whitespace, *e))
         l = e;

  if (l)
    *(l+1) = '\0';
  else
    *str = '\0';

  return str;
}
