/* Simple Plugin API
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

#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <sys/eventfd.h>

#include <spa/support/loop.h>
#include <spa/pod/parser.h>

#include "debug.h"

static const struct spa_type_map *map;

void spa_debug_set_type_map(const struct spa_type_map *m)
{
	map = m;
}

int spa_debug_port_info(const struct spa_port_info *info)
{
	if (info == NULL)
		return -EINVAL;

	fprintf(stderr, "struct spa_port_info %p:\n", info);
	fprintf(stderr, " flags: \t%08x\n", info->flags);

	return 0;
}

int spa_debug_buffer(const struct spa_buffer *buffer)
{
	int i;

	if (buffer == NULL)
		return -EINVAL;

	fprintf(stderr, "spa_buffer %p:\n", buffer);
	fprintf(stderr, " id:      %08X\n", buffer->id);
	fprintf(stderr, " n_metas: %u (at %p)\n", buffer->n_metas, buffer->metas);
	for (i = 0; i < buffer->n_metas; i++) {
		struct spa_meta *m = &buffer->metas[i];
		const char *type_name;

		type_name = spa_type_map_get_type(map, m->type);
		fprintf(stderr, "  meta %d: type %d (%s), data %p, size %d:\n", i, m->type,
			type_name, m->data, m->size);

		if (!strcmp(type_name, SPA_TYPE_META__Header)) {
			struct spa_meta_header *h = m->data;
			fprintf(stderr, "    struct spa_meta_header:\n");
			fprintf(stderr, "      flags:      %08x\n", h->flags);
			fprintf(stderr, "      seq:        %u\n", h->seq);
			fprintf(stderr, "      pts:        %" PRIi64 "\n", h->pts);
			fprintf(stderr, "      dts_offset: %" PRIi64 "\n", h->dts_offset);
		} else if (!strcmp(type_name, SPA_TYPE_META__VideoCrop)) {
			struct spa_meta_video_crop *h = m->data;
			fprintf(stderr, "    struct spa_meta_video_crop:\n");
			fprintf(stderr, "      x:      %d\n", h->x);
			fprintf(stderr, "      y:      %d\n", h->y);
			fprintf(stderr, "      width:  %d\n", h->width);
			fprintf(stderr, "      height: %d\n", h->height);
		} else {
			fprintf(stderr, "    Unknown:\n");
			spa_debug_dump_mem(m->data, m->size);
		}
	}
	fprintf(stderr, " n_datas: \t%u (at %p)\n", buffer->n_datas, buffer->datas);
	for (i = 0; i < buffer->n_datas; i++) {
		struct spa_data *d = &buffer->datas[i];
		fprintf(stderr, "   type:    %d (%s)\n", d->type,
			spa_type_map_get_type(map, d->type));
		fprintf(stderr, "   flags:   %d\n", d->flags);
		fprintf(stderr, "   data:    %p\n", d->data);
		fprintf(stderr, "   fd:      %d\n", d->fd);
		fprintf(stderr, "   offset:  %d\n", d->mapoffset);
		fprintf(stderr, "   maxsize: %u\n", d->maxsize);
		fprintf(stderr, "   chunk:   %p\n", d->chunk);
		fprintf(stderr, "    offset: %d\n", d->chunk->offset);
		fprintf(stderr, "    size:   %u\n", d->chunk->size);
		fprintf(stderr, "    stride: %d\n", d->chunk->stride);
	}
	return 0;
}

int spa_debug_dump_mem(const void *mem, size_t size)
{
	const uint8_t *t = mem;
	int i;

	if (mem == NULL)
		return -EINVAL;

	for (i = 0; i < size; i++) {
		if (i % 16 == 0)
			fprintf(stderr,"%p: ", &t[i]);
		fprintf(stderr,"%02x ", t[i]);
		if (i % 16 == 15 || i == size - 1)
			fprintf(stderr,"\n");
	}
	return 0;
}

static const char *pod_type_names[] = {
	[SPA_POD_TYPE_INVALID] = "invalid",
	[SPA_POD_TYPE_NONE] = "none",
	[SPA_POD_TYPE_BOOL] = "bool",
	[SPA_POD_TYPE_ID] = "id",
	[SPA_POD_TYPE_INT] = "int",
	[SPA_POD_TYPE_LONG] = "long",
	[SPA_POD_TYPE_FLOAT] = "float",
	[SPA_POD_TYPE_DOUBLE] = "double",
	[SPA_POD_TYPE_STRING] = "string",
	[SPA_POD_TYPE_BYTES] = "bytes",
	[SPA_POD_TYPE_RECTANGLE] = "rectangle",
	[SPA_POD_TYPE_FRACTION] = "fraction",
	[SPA_POD_TYPE_BITMAP] = "bitmap",
	[SPA_POD_TYPE_ARRAY] = "array",
	[SPA_POD_TYPE_STRUCT] = "struct",
	[SPA_POD_TYPE_OBJECT] = "object",
	[SPA_POD_TYPE_POINTER] = "pointer",
	[SPA_POD_TYPE_FD] = "fd",
	[SPA_POD_TYPE_PROP] = "prop",
	[SPA_POD_TYPE_POD] = "pod"
};

static void
print_pod_value(uint32_t size, uint32_t type, void *body, int prefix)
{
	switch (type) {
	case SPA_POD_TYPE_BOOL:
		fprintf(stderr,"%-*sBool %d\n", prefix, "", *(int32_t *) body);
		break;
	case SPA_POD_TYPE_ID:
		fprintf(stderr,"%-*sId %d %s\n", prefix, "", *(int32_t *) body,
		       spa_type_map_get_type(map, *(int32_t *) body));
		break;
	case SPA_POD_TYPE_INT:
		fprintf(stderr,"%-*sInt %d\n", prefix, "", *(int32_t *) body);
		break;
	case SPA_POD_TYPE_LONG:
		fprintf(stderr,"%-*sLong %" PRIi64 "\n", prefix, "", *(int64_t *) body);
		break;
	case SPA_POD_TYPE_FLOAT:
		fprintf(stderr,"%-*sFloat %f\n", prefix, "", *(float *) body);
		break;
	case SPA_POD_TYPE_DOUBLE:
		fprintf(stderr,"%-*sDouble %f\n", prefix, "", *(double *) body);
		break;
	case SPA_POD_TYPE_STRING:
		fprintf(stderr,"%-*sString \"%s\"\n", prefix, "", (char *) body);
		break;
	case SPA_POD_TYPE_FD:
		fprintf(stderr,"%-*sFd %d\n", prefix, "", *(int *) body);
		break;
	case SPA_POD_TYPE_POINTER:
	{
		struct spa_pod_pointer_body *b = body;
		fprintf(stderr,"%-*sPointer %s %p\n", prefix, "",
		       map ? spa_type_map_get_type(map, b->type) : "*no map*", b->value);
		break;
	}
	case SPA_POD_TYPE_RECTANGLE:
	{
		struct spa_rectangle *r = body;
		fprintf(stderr,"%-*sRectangle %dx%d\n", prefix, "", r->width, r->height);
		break;
	}
	case SPA_POD_TYPE_FRACTION:
	{
		struct spa_fraction *f = body;
		fprintf(stderr,"%-*sFraction %d/%d\n", prefix, "", f->num, f->denom);
		break;
	}
	case SPA_POD_TYPE_BITMAP:
		fprintf(stderr,"%-*sBitmap\n", prefix, "");
		break;
	case SPA_POD_TYPE_ARRAY:
	{
		struct spa_pod_array_body *b = body;
		void *p;
		fprintf(stderr,"%-*sArray: child.size %d, child.type %d\n", prefix, "",
		       b->child.size, b->child.type);

		SPA_POD_ARRAY_BODY_FOREACH(b, size, p)
			print_pod_value(b->child.size, b->child.type, p, prefix + 2);
		break;
	}
	case SPA_POD_TYPE_STRUCT:
	{
		struct spa_pod *b = body, *p;
		fprintf(stderr,"%-*sStruct: size %d\n", prefix, "", size);
		SPA_POD_FOREACH(b, size, p)
			print_pod_value(p->size, p->type, SPA_POD_BODY(p), prefix + 2);
		break;
	}
	case SPA_POD_TYPE_OBJECT:
	{
		struct spa_pod_object_body *b = body;
		struct spa_pod *p;

		fprintf(stderr,"%-*sObject: size %d, id %s, type %s\n", prefix, "", size,
		       map ? spa_type_map_get_type(map, b->id) : "*no map*",
		       map ? spa_type_map_get_type(map, b->type) : "*no map*");
		SPA_POD_OBJECT_BODY_FOREACH(b, size, p)
			print_pod_value(p->size, p->type, SPA_POD_BODY(p), prefix + 2);
		break;
	}
	case SPA_POD_TYPE_PROP:
	{
		struct spa_pod_prop_body *b = body;
		void *alt;
		int i;

		fprintf(stderr,"%-*sProp: key %s, flags %d\n", prefix, "",
		       map ? spa_type_map_get_type(map, b->key) : "*no map*", b->flags);
		if (b->flags & SPA_POD_PROP_FLAG_UNSET)
			fprintf(stderr,"%-*sUnset (Default):\n", prefix + 2, "");
		else
			fprintf(stderr,"%-*sValue: size %u\n", prefix + 2, "", b->value.size);
		print_pod_value(b->value.size, b->value.type, SPA_POD_BODY(&b->value),
				prefix + 4);

		i = 0;
		switch (b->flags & SPA_POD_PROP_RANGE_MASK) {
		case SPA_POD_PROP_RANGE_NONE:
			break;
		case SPA_POD_PROP_RANGE_MIN_MAX:
			SPA_POD_PROP_ALTERNATIVE_FOREACH(b, size, alt) {
				if (i == 0)
					fprintf(stderr,"%-*sMin: ", prefix + 2, "");
				else if (i == 1)
					fprintf(stderr,"%-*sMax: ", prefix + 2, "");
				else
					break;
				print_pod_value(b->value.size, b->value.type, alt, 0);
				i++;
			}
			break;
		case SPA_POD_PROP_RANGE_STEP:
			SPA_POD_PROP_ALTERNATIVE_FOREACH(b, size, alt) {
				if (i == 0)
					fprintf(stderr,"%-*sMin:  ", prefix + 2, "");
				else if (i == 1)
					fprintf(stderr,"%-*sMax:  ", prefix + 2, "");
				else if (i == 2)
					fprintf(stderr,"%-*sStep: ", prefix + 2, "");
				else
					break;
				print_pod_value(b->value.size, b->value.type, alt, 0);
				i++;
			}
			break;
		case SPA_POD_PROP_RANGE_ENUM:
			SPA_POD_PROP_ALTERNATIVE_FOREACH(b, size, alt) {
				if (i == 0)
					fprintf(stderr,"%-*sEnum:\n", prefix + 2, "");
				print_pod_value(b->value.size, b->value.type, alt, prefix + 4);
				i++;
			}
			break;
		case SPA_POD_PROP_RANGE_FLAGS:
			break;
		}
		break;
	}
	case SPA_POD_TYPE_BYTES:
		fprintf(stderr,"%-*sBytes\n", prefix, "");
		spa_debug_dump_mem(body, size);
		break;
	case SPA_POD_TYPE_NONE:
		fprintf(stderr,"%-*sNone\n", prefix, "");
		spa_debug_dump_mem(body, size);
		break;
	default:
		fprintf(stderr,"unhandled POD type %d\n", type);
		break;
	}
}

static void
print_format_value(uint32_t size, uint32_t type, void *body)
{
	switch (type) {
	case SPA_POD_TYPE_BOOL:
		fprintf(stderr, "%s", *(int32_t *) body ? "true" : "false");
		break;
	case SPA_POD_TYPE_ID:
	{
		const char *str = map ? spa_type_map_get_type(map, *(int32_t *) body) : NULL;
		if (str) {
			const char *h = rindex(str, ':');
			if (h)
				str = h + 1;
		} else {
			str = "unknown";
		}
		fprintf(stderr, "%s", str);
		break;
	}
	case SPA_POD_TYPE_INT:
		fprintf(stderr, "%" PRIi32, *(int32_t *) body);
		break;
	case SPA_POD_TYPE_LONG:
		fprintf(stderr, "%" PRIi64, *(int64_t *) body);
		break;
	case SPA_POD_TYPE_FLOAT:
		fprintf(stderr, "%f", *(float *) body);
		break;
	case SPA_POD_TYPE_DOUBLE:
		fprintf(stderr, "%g", *(double *) body);
		break;
	case SPA_POD_TYPE_STRING:
		fprintf(stderr, "%s", (char *) body);
		break;
	case SPA_POD_TYPE_RECTANGLE:
	{
		struct spa_rectangle *r = body;
		fprintf(stderr, "%" PRIu32 "x%" PRIu32, r->width, r->height);
		break;
	}
	case SPA_POD_TYPE_FRACTION:
	{
		struct spa_fraction *f = body;
		fprintf(stderr, "%" PRIu32 "/%" PRIu32, f->num, f->denom);
		break;
	}
	case SPA_POD_TYPE_BITMAP:
		fprintf(stderr, "Bitmap");
		break;
	case SPA_POD_TYPE_BYTES:
		fprintf(stderr, "Bytes");
		break;
	default:
		break;
	}
}

static int spa_debug_format(const struct spa_pod *format)
{
	int i;
	const char *media_type;
	const char *media_subtype;
	struct spa_pod *pod;
	uint32_t mtype, mstype;

	if (format == NULL || SPA_POD_TYPE(format) != SPA_POD_TYPE_OBJECT)
		return -EINVAL;

	if (spa_pod_object_parse(format, "I", &mtype,
					 "I", &mstype) < 0)
		return -EINVAL;

	media_type = spa_type_map_get_type(map, mtype);
	media_subtype = spa_type_map_get_type(map, mstype);

	fprintf(stderr, "%-6s %s/%s\n", "", rindex(media_type, ':') + 1,
		rindex(media_subtype, ':') + 1);

	SPA_POD_OBJECT_FOREACH((struct spa_pod_object*)format, pod) {
		struct spa_pod_prop *prop;
		const char *key;

		if (pod->type != SPA_POD_TYPE_PROP)
			continue;

		prop = (struct spa_pod_prop *)pod;

		if ((prop->body.flags & SPA_POD_PROP_FLAG_UNSET) &&
		    (prop->body.flags & SPA_POD_PROP_FLAG_OPTIONAL))
			continue;

		key = spa_type_map_get_type(map, prop->body.key);

		fprintf(stderr, "  %20s : (%s) ", rindex(key, ':') + 1,
			pod_type_names[prop->body.value.type]);

		if (!(prop->body.flags & SPA_POD_PROP_FLAG_UNSET)) {
			print_format_value(prop->body.value.size,
					   prop->body.value.type, SPA_POD_BODY(&prop->body.value));
		} else {
			const char *ssep, *esep, *sep;
			void *alt;

			switch (prop->body.flags & SPA_POD_PROP_RANGE_MASK) {
			case SPA_POD_PROP_RANGE_MIN_MAX:
			case SPA_POD_PROP_RANGE_STEP:
				ssep = "[ ";
				sep = ", ";
				esep = " ]";
				break;
			default:
			case SPA_POD_PROP_RANGE_ENUM:
			case SPA_POD_PROP_RANGE_FLAGS:
				ssep = "{ ";
				sep = ", ";
				esep = " }";
				break;
			}

			fprintf(stderr, "%s", ssep);

			i = 0;
			SPA_POD_PROP_ALTERNATIVE_FOREACH(&prop->body, prop->pod.size, alt) {
				if (i > 0)
					fprintf(stderr, "%s", sep);
				print_format_value(prop->body.value.size,
						   prop->body.value.type, alt);
				i++;
			}
			fprintf(stderr, "%s", esep);
		}
		fprintf(stderr, "\n");
	}
	return 0;
}

int spa_debug_pod(const struct spa_pod *pod, uint32_t flags)
{
	int res = 0;

	if (flags & SPA_DEBUG_FLAG_FORMAT)
		res = spa_debug_format(pod);
	else
		print_pod_value(pod->size, pod->type, SPA_POD_BODY(pod), 0);

	return res;
}


int spa_debug_dict(const struct spa_dict *dict)
{
	unsigned int i;

	if (dict == NULL)
		return -EINVAL;

	for (i = 0; i < dict->n_items; i++)
		fprintf(stderr, "          %s = \"%s\"\n", dict->items[i].key,
			dict->items[i].value);

	return 0;
}
