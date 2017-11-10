/* Spa
 * Copyright (C) 2017 Wim Taymans <wim.taymans@gmail.com>
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
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>

#include <spa/support/log.h>
#include <spa/pod/iter.h>

#include <lib/debug.h>

#if 0
/*
  ( "Format",
    ( "video", "raw" ),
    {
      "format":    ( "seu", "I420", ( "I420", "YUY2" ) ),
      "size":      ( "Rru", R(320, 242), ( R(1,1), R(MAX, MAX)) ),
      "framerate": ( "Fru", F(25, 1), ( F(0,1), F(MAX, 1)) )
    }
  )

  ( struct
  { object
  [ array

   1: s = string    :  "value"
      i = int       :  <number>
      l = long      :  <number>
      f = float     :  <float>
      d = double    :  <float>
      b = bool      :  true | false
      R = rectangle : [ <width>, <height> ]
      F = fraction  : [ <num>, <denom> ]

   2: - = default (only default value present)
      e = enum	        : [ <value>, ... ]
      f = flags	        : [ <number> ]
      m = min/max	: [ <min>, <max> ]
      s = min/max/step  : [ <min>, <max>, <step> ]

   3: u = unset		: value is unset, choose from options or default
      o = optional	: value does not need to be set
      r = readonly      : value is read only
      d = deprecated    : value is deprecated
*/
#endif

#define SPA_POD_MAX_DEPTH	16

struct spa_pod_maker {
	struct spa_pod_builder b;
	struct spa_pod_frame frame[SPA_POD_MAX_DEPTH];
	int depth;
};

static inline void spa_pod_maker_init(struct spa_pod_maker *maker,
				      char *data, int size)
{
	spa_pod_builder_init(&maker->b, data, size);
	maker->depth = 0;
}

static const struct {
	char *pat;
	int len;
	int64_t val;
} spa_constants[] = {
	{ "#I_MAX#", strlen("#I_MAX#"), INT32_MAX },
	{ "#I_MIN#", strlen("#I_MIN#"), INT32_MIN },
	{ "#L_MAX#", strlen("#L_MAX#"), INT64_MAX },
	{ "#L_MIN#", strlen("#L_MIN#"), INT64_MIN }
};

static inline int64_t spa_parse_int(const char *str, char **endptr)
{
	int i;

	if (*str != '#')
		return strtoll(str, endptr, 10);

	for (i = 0; i < SPA_N_ELEMENTS(spa_constants); i++) {
		if (strncmp(str, spa_constants[i].pat, spa_constants[i].len) == 0) {
			*endptr = (char *) (str + spa_constants[i].len);
			return spa_constants[i].val;
		}
	}
	return 0;
}

static inline int spa_parse_string(const char *str, char **endptr)
{
	int len;
	for (*endptr = (char *)str+1; **endptr != '\"' && **endptr != '\0'; (*endptr)++);
	len = (*endptr)++ - (str + 1);
	return len;
}

static inline void *
spa_pod_maker_build(struct spa_pod_maker *maker,
		    const char *format, ...)
{
	va_list args;
	const char *start, *strval;
	int64_t intval;
	double doubleval;
	char last;
	struct spa_rectangle *rectval;
	struct spa_fraction *fracval;
	int len;

	va_start(args, format);
	while (*format != '\0') {
		switch (*format) {
		case '[':
			spa_pod_builder_push_struct(&maker->b, &maker->frame[maker->depth++]);
			break;
		case '(':
			spa_pod_builder_push_array(&maker->b, &maker->frame[maker->depth++]);
			break;
		case '{':
			spa_pod_builder_push_object(&maker->b, &maker->frame[maker->depth++], 0, 0);
			break;
		case ']': case '}': case ')':
			spa_pod_builder_pop(&maker->b, &maker->frame[--maker->depth]);
			break;
		case '\"':
			start = format + 1;
			if ((len = spa_parse_string(format, (char **) &format)) < 0)
				return NULL;
			format += strspn(format, " \t\r\n");
			if (*format == ':')
				spa_pod_builder_key_len(&maker->b, start, len);
			else
				spa_pod_builder_string_len(&maker->b, start, len);
			continue;
		case '@':
		case '%':
			last = *format;
			format++;
			switch (*format) {
			case 's':
				strval = va_arg(args, char *);
				spa_pod_builder_string_len(&maker->b, strval, strlen(strval));
				break;
			case 'i':
				spa_pod_builder_int(&maker->b, va_arg(args, int));
				break;
			case 'I':
				spa_pod_builder_id(&maker->b, va_arg(args, int));
				break;
			case 'l':
				spa_pod_builder_long(&maker->b, va_arg(args, int64_t));
				break;
			case 'f':
				spa_pod_builder_float(&maker->b, va_arg(args, double));
				break;
			case 'd':
				spa_pod_builder_double(&maker->b, va_arg(args, double));
				break;
			case 'b':
				spa_pod_builder_bool(&maker->b, va_arg(args, int));
				break;
			case 'z':
			{
				void *ptr  = va_arg(args, void *);
				int len = va_arg(args, int);
				spa_pod_builder_bytes(&maker->b, ptr, len);
				break;
			}
			case 'p':
				spa_pod_builder_pointer(&maker->b, 0, va_arg(args, void *));
				break;
			case 'h':
				spa_pod_builder_fd(&maker->b, va_arg(args, int));
				break;
			case 'a':
			{
				int child_size = va_arg(args, int);
				int child_type = va_arg(args, int);
				int n_elems = va_arg(args, int);
				void *elems = va_arg(args, void *);
				spa_pod_builder_array(&maker->b, child_size, child_type, n_elems, elems);
				break;
			}
			case 'P':
				spa_pod_builder_primitive(&maker->b, va_arg(args, struct spa_pod *));
				break;
			case 'R':
				rectval = va_arg(args, struct spa_rectangle *);
				spa_pod_builder_rectangle(&maker->b, rectval->width, rectval->height);
				break;
			case 'F':
				fracval = va_arg(args, struct spa_fraction *);
				spa_pod_builder_fraction(&maker->b, fracval->num, fracval->denom);
				break;
			}
			if (last == '@') {
				format = va_arg(args, const char *);
				continue;
			}
			break;
		case '0' ... '9': case '-': case '+': case '#':
			start = format;
			intval = spa_parse_int(start, (char **) &format);
			if (*format == '.') {
				doubleval = strtod(start, (char **) &format);
				if (*format == 'f')
					spa_pod_builder_float(&maker->b, doubleval);
				else
					spa_pod_builder_double(&maker->b, doubleval);
				continue;
			}
			switch (*format) {
			case 'x':
				spa_pod_builder_rectangle(&maker->b, intval,
						spa_parse_int(format+1, (char **) &format));
				break;
			case '/':
				spa_pod_builder_fraction(&maker->b, intval,
						spa_parse_int(format+1, (char **) &format));
				break;
			case 'l':
				spa_pod_builder_long(&maker->b, intval);
				format++;
				break;
			default:
				spa_pod_builder_int(&maker->b, intval);
				break;
			}
			continue;
		}
		format++;
	}
	va_end(args);

	return SPA_POD_BUILDER_DEREF(&maker->b, maker->frame[maker->depth].ref, void);
}

static inline int spa_pod_id_to_type(char id)
{
	switch (id) {
	case 'n':
		return SPA_POD_TYPE_NONE;
	case 'b':
		return SPA_POD_TYPE_BOOL;
	case 'I':
		return SPA_POD_TYPE_ID;
	case 'i':
		return SPA_POD_TYPE_INT;
	case 'l':
		return SPA_POD_TYPE_LONG;
	case 'f':
		return SPA_POD_TYPE_FLOAT;
	case 'd':
		return SPA_POD_TYPE_DOUBLE;
	case 's':
		return SPA_POD_TYPE_STRING;
	case 'k':
		return SPA_POD_TYPE_KEY;
	case 'z':
		return SPA_POD_TYPE_BYTES;
	case 'R':
		return SPA_POD_TYPE_RECTANGLE;
	case 'F':
		return SPA_POD_TYPE_FRACTION;
	case 'B':
		return SPA_POD_TYPE_BITMASK;
	case 'A':
		return SPA_POD_TYPE_ARRAY;
	case 'S':
		return SPA_POD_TYPE_STRUCT;
	case 'O':
		return SPA_POD_TYPE_OBJECT;
	case 'M':
		return SPA_POD_TYPE_MAP;
	case 'p':
		return SPA_POD_TYPE_POINTER;
	case 'h':
		return SPA_POD_TYPE_FD;
	case 'V': case 'v':
		return SPA_POD_TYPE_PROP;
	case 'P':
		return SPA_POD_TYPE_POD;
	default:
		return SPA_POD_TYPE_INVALID;
	}
}

enum spa_pod_prop_range {
        SPA_POD_PROP2_RANGE_NONE        = '-',
        SPA_POD_PROP2_RANGE_MIN_MAX     = 'r',
        SPA_POD_PROP2_RANGE_STEP        = 's',
        SPA_POD_PROP2_RANGE_ENUM        = 'e',
        SPA_POD_PROP2_RANGE_FLAGS       = 'f'
};

enum spa_pod_prop_flags {
        SPA_POD_PROP2_FLAG_UNSET        = (1 << 0),
        SPA_POD_PROP2_FLAG_OPTIONAL     = (1 << 1),
        SPA_POD_PROP2_FLAG_READONLY     = (1 << 2),
        SPA_POD_PROP2_FLAG_DEPRECATED   = (1 << 3),
};

struct spa_pod_prop2 {
	enum spa_pod_type type;
	enum spa_pod_prop_range range;
	enum spa_pod_prop_flags flags;
	struct spa_pod *value;
	struct spa_pod *alternatives;
};

static inline int spa_pod_match(struct spa_pod *pod, const char *templ, ...);


static inline int
spa_pod_parse_prop(struct spa_pod *pod, enum spa_pod_type type, struct spa_pod_prop2 *prop)
{
	int res;

	if (SPA_POD_TYPE(pod) == SPA_POD_TYPE_STRUCT) {
		const char *flags;
		char ch;

		if ((res = spa_pod_match(pod,
				"[ %s, %P, %P ]",
				&flags,
				&prop->value,
				&prop->alternatives)) < 0) {
                        printf("can't parse prop chunk %d\n", res);
			return res;
		}
                prop->type = spa_pod_id_to_type(*flags++);
                if (type != SPA_POD_TYPE_POD && type != SPA_POD_TYPE(prop->value)) {
                        printf("prop chunk of wrong type %d != %d\n", SPA_POD_TYPE(prop->value), type);
                        return -1;
                }
                prop->range = *flags++;
                /* flags */
                prop->flags = 0;
                while ((ch = *flags++) != '\0') {
                        switch (ch) {
                        case 'u':
                                prop->flags |= SPA_POD_PROP2_FLAG_UNSET;
                                break;
                        case 'o':
                                prop->flags |= SPA_POD_PROP2_FLAG_OPTIONAL;
                                break;
                        case 'r':
                                prop->flags |= SPA_POD_PROP2_FLAG_READONLY;
                                break;
                        case 'd':
                                prop->flags |= SPA_POD_PROP2_FLAG_DEPRECATED;
                                break;
                        }
                }
	}
	else {
		/* a single value */
                if (type != SPA_POD_TYPE_POD && type != SPA_POD_TYPE(pod)) {
                        printf("prop chunk of wrong type %d != %d\n", SPA_POD_TYPE(prop->value), type);
                        return -1;
                }
		prop->type = SPA_POD_TYPE(pod);
		prop->range = SPA_POD_PROP2_RANGE_NONE;
		prop->flags = 0;
		prop->value = pod;
		prop->alternatives = pod;
	}
	return 0;
}

static inline int
spa_pod_match(struct spa_pod *pod,
	      const char *templ, ...)
{
	struct spa_pod_iter it[SPA_POD_MAX_DEPTH];
	int depth = 0, collected = 0;
	va_list args;
	const char *start;
	int64_t intval, int2val;
	double doubleval;
	char last;
	struct spa_rectangle *rectval;
	struct spa_fraction *fracval;
	int type, len;
	struct spa_pod *current = pod;
	struct spa_pod_prop2 prop;
	bool store, maybe;

	va_start(args, templ);
	while (*templ != '\0') {
		switch (*templ) {
		case '[':
			depth++;
			if (current == NULL ||
			    !spa_pod_iter_struct(&it[depth], current, SPA_POD_SIZE(current)))
				goto done;
			break;
		case '(':
			break;
		case '{':
			depth++;
			if (current == NULL ||
			    !spa_pod_iter_map(&it[depth], current, SPA_POD_SIZE(current)))
				goto done;
			break;
		case ']': case '}': case ')':
			if (depth == 0)
				return -1;
			if (--depth == 0)
				goto done;
			break;
		case '\"':
			start = templ + 1;
			if ((len = spa_parse_string(templ, (char **) &templ)) < 0)
				return -1;
			templ += strspn(templ, " \t\r\n");
			if (*templ == ':') {
				if (SPA_POD_TYPE(it[depth].data) != SPA_POD_TYPE_MAP)
					return -1;
				it[depth].offset = sizeof(struct spa_pod_map);
				/* move to key */
				while (spa_pod_iter_has_next(&it[depth])) {
					current = spa_pod_iter_next(&it[depth]);
					if (SPA_POD_TYPE(current) == SPA_POD_TYPE_KEY &&
					    strncmp(SPA_POD_CONTENTS_CONST(struct spa_pod_key, current),
						    start, len) == 0)
						break;
					current = NULL;
				}
			}
			else {
				if (current == NULL || SPA_POD_TYPE(current) != SPA_POD_TYPE_STRING ||
				    strncmp(SPA_POD_CONTENTS_CONST(struct spa_pod_string, current),
					    start, len) != 0)
					goto done;
			}
			break;
		case '@':
		case '%':
			last = *templ;
			if (*++templ == '\0')
				return -1;

			store = *templ != '*';
			if (!store)
				if (*++templ == '\0')
					return -1;

			maybe = *templ == '?';
			if (maybe)
				if (*++templ == '\0')
					return -1;

			if (*templ == 'V' || *templ == 'v') {
				char t = *templ;
				templ++;
				type = spa_pod_id_to_type(*templ);

				if (current == NULL)
					goto no_current;

				if (spa_pod_parse_prop(current, type, &prop) < 0)
					return -1;

				if (t == 'v') {
					if (prop.flags & SPA_POD_PROP2_FLAG_UNSET) {
						if (store)
							va_arg(args, void *);
						goto skip;
					}
					else
						current = prop.value;
				}
				else {
					collected++;
					*va_arg(args, struct spa_pod_prop2 *) = prop;
					goto skip;
				}
			}

		      no_current:
			type = spa_pod_id_to_type(*templ);
			if (current == NULL || (type != SPA_POD_TYPE_POD && type != SPA_POD_TYPE(current))) {
				if (!maybe)
					return -1;
				if (store)
					va_arg(args, void *);
				goto skip;
			}
			if (!store)
				goto skip;

			collected++;

			switch (*templ) {
			case 'n': case 'A': case 'S': case 'O': case 'M': case 'P':
				*va_arg(args, struct spa_pod **) = current;
				break;
			case 'b':
			case 'i':
			case 'I':
				*va_arg(args, int32_t *) = SPA_POD_VALUE(struct spa_pod_int, current);
				break;
			case 'l':
				*va_arg(args, int64_t *) = SPA_POD_VALUE(struct spa_pod_long, current);
				break;
			case 'f':
				*va_arg(args, float *) = SPA_POD_VALUE(struct spa_pod_float, current);
				break;
			case 'd':
				*va_arg(args, double *) = SPA_POD_VALUE(struct spa_pod_double, current);
				break;
			case 's':
			case 'k':
				*va_arg(args, char **) = SPA_POD_CONTENTS(struct spa_pod_string, current);
				break;
			case 'z':
				*va_arg(args, void **) = SPA_POD_CONTENTS(struct spa_pod_bytes, current);
				*va_arg(args, uint32_t *) = SPA_POD_BODY_SIZE(current);
				break;
			case 'R':
				*va_arg(args, struct spa_rectangle *) =
					SPA_POD_VALUE(struct spa_pod_rectangle, current);
				break;
			case 'F':
				*va_arg(args, struct spa_fraction *) =
					SPA_POD_VALUE(struct spa_pod_fraction, current);
				break;
			case 'p':
			{
	                        struct spa_pod_pointer_body *b = SPA_POD_BODY(current);
				*va_arg(args, void **) = b->value;
				break;
			}
			case 'h':
				*va_arg(args, int *) = SPA_POD_VALUE(struct spa_pod_fd, current);
				break;
			default:
				va_arg(args, void *);
				break;
			}

		      skip:
			if (last == '@') {
				templ = va_arg(args, void *);
				goto next;
			}
			break;
		case '0' ... '9': case '-': case '+': case '#':
			start = templ;
			intval = spa_parse_int(start, (char **) &templ);
			if (*templ == '.') {
				doubleval = strtod(start, (char **) &templ);
				if (*templ == 'f') {
					if (current == NULL ||
					    SPA_POD_TYPE(current) != SPA_POD_TYPE_FLOAT ||
					    doubleval != SPA_POD_VALUE(struct spa_pod_float, current))
						goto done;
					break;
				}
				else if (current == NULL ||
				    SPA_POD_TYPE(current) != SPA_POD_TYPE_DOUBLE ||
				    doubleval != SPA_POD_VALUE(struct spa_pod_double, current))
					goto done;
				goto next;
			}
			switch (*templ) {
			case 'x':
				if (current == NULL ||
				    SPA_POD_TYPE(current) != SPA_POD_TYPE_RECTANGLE)
					goto done;

				rectval = &SPA_POD_VALUE(struct spa_pod_rectangle, current);
				int2val = spa_parse_int(templ+1, (char **) &templ);
				if (rectval->width != intval || rectval->height != int2val)
					goto done;
				goto next;
			case '/':
				if (current == NULL ||
				    SPA_POD_TYPE(current) != SPA_POD_TYPE_FRACTION)
					goto done;

				fracval = &SPA_POD_VALUE(struct spa_pod_fraction, current);
				int2val = spa_parse_int(templ+1, (char **) &templ);
				if (fracval->num != intval || fracval->denom != int2val)
					goto done;
				goto next;
			case 'l':
				if (current == NULL ||
				    SPA_POD_TYPE(current) != SPA_POD_TYPE_LONG ||
				    SPA_POD_VALUE(struct spa_pod_long, current) != intval)
					goto done;
				break;
			default:
				if (current == NULL ||
				    SPA_POD_TYPE(current) != SPA_POD_TYPE_INT ||
				    SPA_POD_VALUE(struct spa_pod_int, current) != intval)
					goto done;
				break;
			}
			break;
		case ' ': case '\n': case '\t': case '\r': case ',':
			templ++;
			continue;
		}
		templ++;

	      next:
		if (spa_pod_iter_has_next(&it[depth]))
			current = spa_pod_iter_next(&it[depth]);
		else
			current = NULL;
	}
	va_end(args);

      done:
	return collected;
}

static int test_match(const char *fmt)
{
	const char *media_type, *media_subtype, *format;
	int rate = -1, res;
	struct spa_pod_prop2 channels;
	struct spa_pod *pod;
	struct spa_pod_maker m = { 0, };
	char buffer[4096];

	spa_pod_maker_init(&m, buffer, sizeof(buffer));
	pod = spa_pod_maker_build(&m, fmt);
	spa_debug_pod(pod);

	res = spa_pod_match(pod,
		"[ \"Format\", "
		"  [ @s",&media_type," @s",&media_subtype," ], "
		"  { "
		"    \"rate\":        @vi", &rate,
		"    \"format\":      @vs", &format,
		"    \"channels\":    @VP", &channels,
		"    \"foo\":         @?VP", &channels,
		"  } "
		"]");

	printf("collected %d\n", res);
	printf("media type %s\n", media_type);
	printf("media subtype %s\n", media_subtype);
	printf("media rate %d\n", rate);
	printf("media format %s\n", format);
	printf("media channels: %d %c %04x\n",channels.type, channels.range, channels.flags);
	spa_debug_pod(channels.value);
	spa_debug_pod(channels.alternatives);
	return 0;
}

int main(int argc, char *argv[])
{
	struct spa_pod_maker m = { 0, };
	char buffer[4096];
	struct spa_pod *fmt;

	spa_pod_maker_init(&m, buffer, sizeof(buffer));
	fmt = spa_pod_maker_build(&m,
				"[ \"Format\", "
				" [\"video\", \"raw\" ], "
				" { "
				"   \"format\":    [ \"eu\", \"I420\", [ \"I420\",\"YUY2\" ] ], "
				"   \"size\":      [ \"ru\", 320x242, [ 1x1, #I_MAX#x#I_MAX# ] ], "
				"   \"framerate\": [ \"ru\", 25/1, [ 0/1, #I_MAX#/1 ] ] "
				" } "
				"] ");
	spa_debug_pod(fmt);

	spa_pod_maker_init(&m, buffer, sizeof(buffer));
	fmt = spa_pod_maker_build(&m,
				"[ \"Format\", "
				" [\"video\", %s ], "
				" { "
				"   \"format\":    [ \"eu\", \"I420\", [ %s, \"YUY2\" ] ], "
				"   \"size\":      [ \"ru\", 320x242, [ %R, #I_MAX#x#I_MAX# ] ], "
				"   \"framerate\": [ \"ru\", %F, [ 0/1, #I_MAX#/1 ] ] "
				" } "
				"] ",
					"raw",
					"I420",
					&(struct spa_rectangle){ 1, 1 },
					&(struct spa_fraction){ 25, 1 }
				);
	spa_debug_pod(fmt);

	{
		const char *format = "S16";
		int rate = 44100, channels = 2;

		spa_pod_maker_init(&m, buffer, sizeof(buffer));
		fmt = spa_pod_maker_build(&m,
                                "[ \"Format\", "
                                " [\"audio\", \"raw\" ], "
                                " { "
                                "   \"format\":   [@s", format, "] "
                                "   \"rate\":     [@i", rate, "] "
                                "   \"channels\": [@i", channels, "] "
                                "   \"rect\":     [@R", &SPA_RECTANGLE(32, 22), "] "
                                " } "
                                "] ");
		spa_debug_pod(fmt);
	}

	{
		const char *format = "S16";
		int rate = 44100, channels = 2;
		struct spa_rectangle rects[3] = { { 1, 1 }, { 2, 2},  {3, 3}};
		struct spa_pod_int pod = SPA_POD_INT_INIT(12);
		uint8_t bytes[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13 };

		spa_pod_maker_init(&m, buffer, sizeof(buffer));
		spa_pod_maker_build(&m,
                                "[ \"Format\", "
                                " [\"audio\", \"raw\" ], ");
		fmt = spa_pod_maker_build(&m,
                                " { "
                                "   \"format\":   [ %s ] "
                                "   \"rate\":     [ %i, ( 44100, 48000, 96000 ) ]"
                                "   \"foo\":      %i, ( 1.1, 2.2, 3.2  )"
                                "   \"baz\":      ( 1.1f, 2.2f, 3.2f )"
                                "   \"bar\":      ( 1x1, 2x2, 3x2 )"
                                "   \"faz\":      ( 1/1, 2/2, 3/2 )"
                                "   \"wha\":      %a, "
                                "   \"fuz\":      %P, "
//                                "   \"fur\":      ( (1, 2), (7, 8), (7, 5) ) "
//                                "   \"fur\":      ( [1, 2], [7, 8], [7, 5] ) "
                                "   \"buz\":      %z, "
                                "   \"boo\":      %p, "
                                "   \"foz\":      %h, "
                                " } "
                                "] ", format, rate, channels,
				sizeof(struct spa_rectangle), SPA_POD_TYPE_RECTANGLE, 3, rects,
				&pod,
				bytes, sizeof(bytes),
				fmt,
				STDOUT_FILENO);
		spa_debug_pod(fmt);
	}

	spa_pod_maker_init(&m, buffer, sizeof(buffer));
	fmt = spa_pod_maker_build(&m,
				"[ \"Format\", "
				" [\"video\", %s ], "
				" { "
				"   \"format\":    [ \"eu\", \"I420\", [ %s, \"YUY2\" ] ], "
				"   \"size\":      [ \"ru\", 320x242, [ %R, #I_MAX#x#I_MAX# ] ], "
				"   \"framerate\": [ \"ru\", %F, [ 0/1, #I_MAX#/1 ] ] "
				" } "
				"] ",
					"raw",
					"I420",
					&(struct spa_rectangle){ 1, 1 },
					&(struct spa_fraction){ 25, 1 }
				);
	spa_debug_pod(fmt);

	{
		const char *subtype = NULL, *format = NULL;
		struct spa_pod *pod = NULL;
		struct spa_rectangle rect = { 0, 0 };
		struct spa_fraction frac = { 0, 0 };
		int res;

		res = spa_pod_match(fmt,
				"[ \"Format\", "
				" [\"video\", %s ], "
				" { "
				"   \"format\":    [ %*s, %*s, [ %s, %*s ] ], "
				"   \"size\":      [ \"ru\", 320x242, [ %R, %P ] ], "
				"   \"framerate\": [ %*P, %F, %*S ] "
				" } "
				"] ",
				&subtype,
				&format,
				&rect,
				&pod,
				&frac);

		printf("collected %d\n", res);
		printf("media type %s\n", subtype);
		printf("media format %s\n", format);
		printf("media size %dx%d\n", rect.width, rect.height);
		printf("media size pod\n");
		spa_debug_pod(pod);
		printf("media framerate %d/%d\n", frac.num, frac.denom);

		res = spa_pod_match(fmt,
				"[ \"Format\", "
				" [\"video\", @s", &subtype, " ], "
				" { "
				"   \"format\":    [ %*s, %*s, [ @s", &format, ", %*s ] ], "
				"   \"size\":      [ \"ru\", 320x242, [ @R",&rect,", @P",&pod," ] ], "
				"   \"framerate\": [ %*P, @F",&frac,", %*S ] "
				" } "
				"] ");

		printf("collected %d\n", res);
		printf("media type %s\n", subtype);
		printf("media format %s\n", format);
		printf("media size %dx%d\n", rect.width, rect.height);
		printf("media size pod\n");
		spa_debug_pod(pod);
		printf("media framerate %d/%d\n", frac.num, frac.denom);

		res = spa_pod_match(fmt,
				"[ \"Format\", "
				" [\"video\", @s", &subtype, " ], "
				" { "
				"   \"format\":    [ %*s, %*s, [ @s", &format, ", %*s ] ], "
				"   \"size\":      [ \"ru\", 320x242, [ @R",&rect,", @P",&pod," ] ], "
				"   \"framerate\": [ %*P, @F",&frac,", %*S ] "
				" } "
				"] ");

	}

	test_match("[ \"Format\", "
		" [\"audio\", \"raw\" ], "
		" { "
		"   \"format\":    [ \"se\", \"S16\", [ \"S16\", \"F32\" ] ], "
		"   \"rate\":      [ \"iru\", 44100, [ 1, 192000  ] ], "
		"   \"channels\":  [ \"ir\", 2, [ 1, #I_MAX# ]] "
		" } "
		"] ");

	test_match(
		"[ \"Format\", "
		"  [ \"audio\", \"raw\"], "
                "  { "
                "    \"format\":      \"S16LE\", "
                "    \"rate\":        44100, "
                "    \"channels\":    2 "
                "  }"
                "]");

	return 0;
}
