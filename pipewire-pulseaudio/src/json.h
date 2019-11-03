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

#include <stdbool.h>

#define PA_DOUBLE_IS_EQUAL(x, y) (((x) - (y)) < 0.000001 && ((x) - (y)) > -0.000001)

typedef enum {
    PA_JSON_TYPE_INIT = 0,
    PA_JSON_TYPE_NULL,
    PA_JSON_TYPE_INT,
    PA_JSON_TYPE_DOUBLE,
    PA_JSON_TYPE_BOOL,
    PA_JSON_TYPE_STRING,
    PA_JSON_TYPE_ARRAY,
    PA_JSON_TYPE_OBJECT,
} pa_json_type;

typedef struct pa_json_object pa_json_object;

pa_json_object* pa_json_parse(const char *str);
pa_json_type pa_json_object_get_type(const pa_json_object *obj);
void pa_json_object_free(pa_json_object *obj);

/* All pointer members that are returned are valid while the corresponding object is valid */

int pa_json_object_get_int(const pa_json_object *o);
double pa_json_object_get_double(const pa_json_object *o);
bool pa_json_object_get_bool(const pa_json_object *o);
const char* pa_json_object_get_string(const pa_json_object *o);

const pa_json_object* pa_json_object_get_object_member(const pa_json_object *o, const char *name);

int pa_json_object_get_array_length(const pa_json_object *o);
const pa_json_object* pa_json_object_get_array_member(const pa_json_object *o, int index);

bool pa_json_object_equal(const pa_json_object *o1, const pa_json_object *o2);
