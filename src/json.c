/***
  This file is part of PulseAudio.

  Copyright 2016 Arun Raghavan <mail@arunraghavan.net>

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

#include <math.h>

#include <pulse/xmalloc.h>

#include <pipewire/array.h>
#include <pipewire/properties.h>

#include "internal.h"
#include "json.h"
#include "strbuf.h"

#define MAX_NESTING_DEPTH 20 /* Arbitrary number to make sure we don't have a stack overflow */

typedef struct pa_json_item {
	char *key;
	pa_json_object *value;
} pa_json_item;

struct pa_json_object {
    pa_json_type type;

    union {
        int int_value;
        double double_value;
        bool bool_value;
        char *string_value;
        struct pw_array values; /* objects */
    };
};

static void clear_array(struct pw_array *array)
{
    pa_json_object **value;
    pw_array_for_each(value, array)
	    pa_json_object_free(*value);
    pw_array_clear(array);
}

static void clear_item(pa_json_item *item)
{
    free(item->key);
    pa_json_object_free(item->value);
}

static void clear_object(struct pw_array *array)
{
    pa_json_item *item;
    pw_array_for_each(item, array)
	    clear_item(item);
    pw_array_clear(array);
}

static const char* parse_value(const char *str, const char *end, pa_json_object **obj, unsigned int depth);

static pa_json_object* json_object_new(void) {
    pa_json_object *obj;

    obj = pa_xnew0(pa_json_object, 1);

    return obj;
}

static bool is_whitespace(char c) {
    return c == '\t' || c == '\n' || c == '\r' || c == ' ';
}

static bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

static bool is_end(const char c, const char *end) {
    if (!end)
        return c == '\0';
    else  {
        while (*end) {
            if (c == *end)
                return true;
            end++;
        }
    }

    return false;
}

static const char* consume_string(const char *str, const char *expect) {
    while (*expect) {
        if (*str != *expect)
            return NULL;

        str++;
        expect++;
    }

    return str;
}

static const char* parse_null(const char *str, pa_json_object *obj) {
    str = consume_string(str, "null");

    if (str)
        obj->type = PA_JSON_TYPE_NULL;

    return str;
}

static const char* parse_boolean(const char *str, pa_json_object *obj) {
    const char *tmp;

    tmp = consume_string(str, "true");

    if (tmp) {
        obj->type = PA_JSON_TYPE_BOOL;
        obj->bool_value = true;
    } else {
        tmp = consume_string(str, "false");

        if (str) {
            obj->type = PA_JSON_TYPE_BOOL;
            obj->bool_value = false;
        }
    }

    return tmp;
}

static const char* parse_string(const char *str, pa_json_object *obj) {
    pa_strbuf *buf = pa_strbuf_new();

    str++; /* Consume leading '"' */

    while (*str && *str != '"') {
        if (*str != '\\') {
            /* We only accept ASCII printable characters. */
            if (*str < 0x20 || *str > 0x7E) {
                pa_log("Invalid non-ASCII character: 0x%x", (unsigned int) *str);
                goto error;
            }

            /* Normal character, juts consume */
            pa_strbuf_putc(buf, *str);
        } else {
            /* Need to unescape */
            str++;

            switch (*str) {
                case '"':
                case '\\':
                case '/':
                    pa_strbuf_putc(buf, *str);
                    break;

                case 'b':
                    pa_strbuf_putc(buf, '\b' /* backspace */);
                    break;

                case 'f':
                    pa_strbuf_putc(buf, '\f' /* form feed */);
                    break;

                case 'n':
                    pa_strbuf_putc(buf, '\n' /* new line */);
                    break;

                case 'r':
                    pa_strbuf_putc(buf, '\r' /* carriage return */);
                    break;

                case 't':
                    pa_strbuf_putc(buf, '\t' /* horizontal tab */);
                    break;

                case 'u':
                    pa_log("Unicode code points are currently unsupported");
                    goto error;

                default:
                    pa_log("Unexepcted escape value: %c", *str);
                    goto error;
            }
        }

        str++;
    }

    if (*str != '"') {
        pa_log("Failed to parse remainder of string: %s", str);
        goto error;
    }

    str++;

    obj->type = PA_JSON_TYPE_STRING;
    obj->string_value = pa_strbuf_to_string_free(buf);

    return str;

error:
    pa_strbuf_free(buf);
    return NULL;
}

static const char* parse_number(const char *str, pa_json_object *obj) {
    bool negative = false, has_fraction = false, has_exponent = false, valid = false;
    unsigned int integer = 0;
    unsigned int fraction = 0;
    unsigned int fraction_digits = 0;
    int exponent = 0;

    if (*str == '-') {
        negative = true;
        str++;
    }

    if (*str == '0') {
        valid = true;
        str++;
        goto fraction;
    }

    while (is_digit(*str)) {
        valid = true;

        if (integer > ((negative ? INT_MAX : UINT_MAX) / 10)) {
            pa_log("Integer overflow while parsing number");
            goto error;
        }

        integer = (integer * 10) + (*str - '0');
        str++;
    }

fraction:

    if (!valid) {
        pa_log("Missing digits while parsing number");
        goto error;
    }

    if (*str == '.') {
        has_fraction = true;
        str++;
        valid = false;

        while (is_digit(*str)) {
            valid = true;

            if (fraction > (UINT_MAX / 10)) {
                pa_log("Integer overflow while parsing fractional part of number");
                goto error;
            }

            fraction = (fraction * 10) + (*str - '0');
            fraction_digits++;
            str++;
        }

        if (!valid) {
            pa_log("No digit after '.' while parsing fraction");
            goto error;
        }
    }

    if (*str == 'e' || *str == 'E') {
        bool exponent_negative = false;

        has_exponent = true;
        str++;
        valid = false;

        if (*str == '-') {
            exponent_negative = true;
            str++;
        } else if (*str == '+')
            str++;

        while (is_digit(*str)) {
            valid = true;

            if (exponent > (INT_MAX / 10)) {
                pa_log("Integer overflow while parsing exponent part of number");
                goto error;
            }

            exponent = (exponent * 10) + (*str - '0');
            str++;
        }

        if (!valid) {
            pa_log("No digit in exponent while parsing fraction");
            goto error;
        }

        if (exponent_negative)
            exponent *= -1;
    }

    if (has_fraction || has_exponent) {
        obj->type = PA_JSON_TYPE_DOUBLE;
        obj->double_value =
            (negative ? -1.0 : 1.0) * (integer + (double) fraction / pow(10, fraction_digits)) * pow(10, exponent);
    } else {
        obj->type = PA_JSON_TYPE_INT;
        obj->int_value = (negative ? -1 : 1) * integer;
    }

    return str;

error:
    return NULL;
}

static const char *parse_object(const char *str, pa_json_object *obj, unsigned int depth) {
    pa_json_object *name = NULL, *value = NULL;
    pa_json_item *item;

    obj->values = PW_ARRAY_INIT(64);

    while (*str != '}') {
        str++; /* Consume leading '{' or ',' */

        str = parse_value(str, ":", &name, depth + 1);
        if (!str || pa_json_object_get_type(name) != PA_JSON_TYPE_STRING) {
            pa_log("Could not parse key for object");
            goto error;
        }

        /* Consume the ':' */
        str++;

        str = parse_value(str, ",}", &value, depth + 1);
        if (!str) {
            pa_log("Could not parse value for object");
            goto error;
        }

	item = pw_array_add(&obj->values, sizeof(pa_json_item));
	item->key = strdup(pa_json_object_get_string(name));
	item->value = value;
        pa_json_object_free(name);

        name = NULL;
        value = NULL;
    }

    /* Drop trailing '}' */
    str++;

    /* We now know the value was correctly parsed */
    obj->type = PA_JSON_TYPE_OBJECT;

    return str;

error:
    clear_object(&obj->values);

    if (name)
        pa_json_object_free(name);
    if (value)
        pa_json_object_free(value);

    return NULL;
}

static const char *parse_array(const char *str, pa_json_object *obj, unsigned int depth) {
    pa_json_object *value;

    obj->values = PW_ARRAY_INIT(64);

    while (*str != ']') {
        str++; /* Consume leading '[' or ',' */

        /* Need to chew up whitespaces as a special case to deal with the
         * possibility of an empty array */
        while (is_whitespace(*str))
            str++;

        if (*str == ']')
            break;

        str = parse_value(str, ",]", &value, depth + 1);
        if (!str) {
            pa_log("Could not parse value for array");
            goto error;
        }

        pw_array_add_ptr(&obj->values, value);
    }

    /* Drop trailing ']' */
    str++;

    /* We now know the value was correctly parsed */
    obj->type = PA_JSON_TYPE_ARRAY;

    return str;

error:
    clear_array(&obj->values);
    return NULL;
}

typedef enum {
    JSON_PARSER_STATE_INIT,
    JSON_PARSER_STATE_FINISH,
} json_parser_state;

static const char* parse_value(const char *str, const char *end, pa_json_object **obj, unsigned int depth) {
    json_parser_state state = JSON_PARSER_STATE_INIT;
    pa_json_object *o;

    pa_assert(str != NULL);

    o = json_object_new();

    if (depth > MAX_NESTING_DEPTH) {
        pa_log("Exceeded maximum permitted nesting depth of objects (%u)", MAX_NESTING_DEPTH);
        goto error;
    }

    while (!is_end(*str, end)) {
        switch (state) {
            case JSON_PARSER_STATE_INIT:
                if (is_whitespace(*str)) {
                    str++;
                } else if (*str == 'n') {
                    str = parse_null(str, o);
                    state = JSON_PARSER_STATE_FINISH;
                } else if (*str == 't' || *str == 'f') {
                    str = parse_boolean(str, o);
                    state = JSON_PARSER_STATE_FINISH;
                } else if (*str == '"') {
                    str = parse_string(str, o);
                    state = JSON_PARSER_STATE_FINISH;
                } else if (is_digit(*str) || *str == '-') {
                    str = parse_number(str, o);
                    state = JSON_PARSER_STATE_FINISH;
                } else if (*str == '{') {
                    str = parse_object(str, o, depth);
                    state = JSON_PARSER_STATE_FINISH;
                } else if (*str == '[') {
                    str = parse_array(str, o, depth);
                    state = JSON_PARSER_STATE_FINISH;
                } else {
                    pa_log("Invalid JSON string: %s", str);
                    goto error;
                }

                if (!str)
                    goto error;

                break;

            case JSON_PARSER_STATE_FINISH:
                /* Consume trailing whitespaces */
                if (is_whitespace(*str)) {
                    str++;
                } else {
                    goto error;
                }
        }
    }

    if (pa_json_object_get_type(o) == PA_JSON_TYPE_INIT) {
        /* We didn't actually get any data */
        pa_log("No data while parsing json string: '%s' till '%s'", str, pa_strnull(end));
        goto error;
    }

    *obj = o;

    return str;

error:
    pa_json_object_free(o);
    return NULL;
}


pa_json_object* pa_json_parse(const char *str) {
    pa_json_object *obj;

    str = parse_value(str, NULL, &obj, 0);

    if (!str) {
        pa_log("JSON parsing failed");
        return NULL;
    }

    if (*str != '\0') {
        pa_log("Unable to parse complete JSON string, remainder is: %s", str);
        pa_json_object_free(obj);
        return NULL;
    }

    return obj;
}

pa_json_type pa_json_object_get_type(const pa_json_object *obj) {
    return obj->type;
}

void pa_json_object_free(pa_json_object *obj) {

    switch (pa_json_object_get_type(obj)) {
        case PA_JSON_TYPE_INIT:
        case PA_JSON_TYPE_INT:
        case PA_JSON_TYPE_DOUBLE:
        case PA_JSON_TYPE_BOOL:
        case PA_JSON_TYPE_NULL:
            break;

        case PA_JSON_TYPE_STRING:
            pa_xfree(obj->string_value);
            break;

        case PA_JSON_TYPE_OBJECT:
            clear_object(&obj->values);
            break;

        case PA_JSON_TYPE_ARRAY:
            clear_array(&obj->values);
            break;

        default:
            pa_assert_not_reached();
    }

    pa_xfree(obj);
}

int pa_json_object_get_int(const pa_json_object *o) {
    pa_assert(pa_json_object_get_type(o) == PA_JSON_TYPE_INT);
    return o->int_value;
}

double pa_json_object_get_double(const pa_json_object *o) {
    pa_assert(pa_json_object_get_type(o) == PA_JSON_TYPE_DOUBLE);
    return o->double_value;
}

bool pa_json_object_get_bool(const pa_json_object *o) {
    pa_assert(pa_json_object_get_type(o) == PA_JSON_TYPE_BOOL);
    return o->bool_value;
}

const char* pa_json_object_get_string(const pa_json_object *o) {
    pa_assert(pa_json_object_get_type(o) == PA_JSON_TYPE_STRING);
    return o->string_value;
}

const pa_json_object* pa_json_object_get_object_member(const pa_json_object *o, const char *name) {
    pa_json_item *item;
    pa_assert(pa_json_object_get_type(o) == PA_JSON_TYPE_OBJECT);
    pw_array_for_each(item, &o->values) {
        if (pa_streq(item->key, name))
            return item->value;
    }
    return NULL;
}

int pa_json_object_get_array_length(const pa_json_object *o) {
    pa_assert(pa_json_object_get_type(o) == PA_JSON_TYPE_ARRAY);
    return pw_array_get_len(&o->values, const pa_json_object*);
}

const pa_json_object* pa_json_object_get_array_member(const pa_json_object *o, int index) {
    pa_assert(pa_json_object_get_type(o) == PA_JSON_TYPE_ARRAY);
    return pw_array_get_unchecked_s(&o->values, index, sizeof(pa_json_object*),
		    const pa_json_object);
}

bool pa_json_object_equal(const pa_json_object *o1, const pa_json_object *o2) {
    int i;

    if (pa_json_object_get_type(o1) != pa_json_object_get_type(o2))
        return false;

    switch (pa_json_object_get_type(o1)) {
        case PA_JSON_TYPE_NULL:
            return true;

        case PA_JSON_TYPE_BOOL:
            return o1->bool_value == o2->bool_value;

        case PA_JSON_TYPE_INT:
            return o1->int_value == o2->int_value;

        case PA_JSON_TYPE_DOUBLE:
            return PA_DOUBLE_IS_EQUAL(o1->double_value, o2->double_value);

        case PA_JSON_TYPE_STRING:
            return pa_streq(o1->string_value, o2->string_value);

        case PA_JSON_TYPE_ARRAY:
            if (pa_json_object_get_array_length(o1) != pa_json_object_get_array_length(o2))
                return false;

            for (i = 0; i < pa_json_object_get_array_length(o1); i++) {
                if (!pa_json_object_equal(pa_json_object_get_array_member(o1, i),
                            pa_json_object_get_array_member(o2, i)))
                    return false;
            }

            return true;

        case PA_JSON_TYPE_OBJECT: {
            const pa_json_object *value;
            const pa_json_item *item;

            if (o1->values.size != o2->values.size)
                return false;

            pw_array_for_each(item, &o1->values) {
                value = pa_json_object_get_object_member(o2, item->key);
                if (!value || !pa_json_object_equal(item->value, value))
                    return false;
            }

            return true;
        }

        default:
            pa_assert_not_reached();
    }
}
