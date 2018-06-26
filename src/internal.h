/* PipeWire
 * Copyright (C) 2018 Wim Taymans <wim.taymans@gmail.com>
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

#ifndef __PIPEWIRE_PULSEAUDIO_INTERNAL_H__
#define __PIPEWIRE_PULSEAUDIO_INTERNAL_H__

#include <string.h>

#include <spa/utils/defs.h>
#include <spa/utils/hook.h>
#include <spa/utils/ringbuffer.h>
#include <spa/param/format-utils.h>
#include <spa/param/audio/format-utils.h>

#include <pulse/stream.h>
#include <pulse/format.h>
#include <pulse/subscribe.h>

#include <pipewire/utils.h>
#include <pipewire/interfaces.h>
#include <pipewire/log.h>

#define PA_MAX_FORMATS (PA_ENCODING_MAX)

#ifdef __cplusplus
extern "C" {
#endif

#define pa_streq(a,b)		(!strcmp((a),(b)))
#define pa_strneq(a,b,n)	(!strncmp((a),(b),(n)))

#define PA_UNLIKELY		SPA_UNLIKELY
#define PA_LIKELY		SPA_LIKELY
#define PA_MIN			SPA_MIN
#define PA_MAX			SPA_MAX
#define pa_assert		spa_assert
#define pa_assert_se		spa_assert
#define pa_return_val_if_fail	spa_return_val_if_fail
#define pa_assert_not_reached	spa_assert_not_reached

#define PA_INT_TYPE_SIGNED(type) (!!((type) 0 > (type) -1))

#define PA_INT_TYPE_HALF(type) ((type) 1 << (sizeof(type)*8 - 2))

#define PA_INT_TYPE_MAX(type)                                          \
    ((type) (PA_INT_TYPE_SIGNED(type)                                  \
             ? (PA_INT_TYPE_HALF(type) - 1 + PA_INT_TYPE_HALF(type))   \
             : (type) -1))

#define PA_INT_TYPE_MIN(type)                                          \
    ((type) (PA_INT_TYPE_SIGNED(type)                                  \
             ? (-1 - PA_INT_TYPE_MAX(type))                            \
             : (type) 0))


#ifdef __GNUC__
#define PA_CLAMP_UNLIKELY(x, low, high)                                 \
	__extension__ ({                                                    \
		typeof(x) _x = (x);                                         \
		typeof(low) _low = (low);                                   \
		typeof(high) _high = (high);                                \
		(PA_UNLIKELY(_x > _high) ? _high : (PA_UNLIKELY(_x < _low) ? _low : _x)); \
	})
#else
#define PA_CLAMP_UNLIKELY(x, low, high) (PA_UNLIKELY((x) > (high)) ? (high) : (PA_UNLIKELY((x) < (low)) ? (low) : (x)))
#endif


#define pa_init_i18n()
#define _(String)		(String)
#define N_(String)		(String)

#define pa_snprintf		snprintf
#define pa_strip(n)		pw_strip(n,"\n\r \t")

#define pa_log			pw_log_info
#define pa_log_debug		pw_log_debug
#define pa_log_warn		pw_log_warn

static inline void* PA_ALIGN_PTR(const void *p) {
    return (void*) (((size_t) p) & ~(sizeof(void*) - 1));
}

/* Rounds up */
static inline size_t PA_ALIGN(size_t l) {
    return ((l + sizeof(void*) - 1) & ~(sizeof(void*) - 1));
}

static inline const char *pa_strnull(const char *x) {
    return x ? x : "(null)";
}

int pa_context_set_error(pa_context *c, int error);

#define PA_CHECK_VALIDITY(context, expression, error)			\
do {									\
	if (!(expression)) {						\
		fprintf(stderr, "'%s' failed at %s:%u %s()",		\
			#expression , __FILE__, __LINE__, __func__);	\
		return -pa_context_set_error((context), (error));	\
	}								\
} while(false)

#define PA_CHECK_VALIDITY_RETURN_ANY(context, expression, error, value)	\
do {									\
	if (!(expression)) {						\
		fprintf(stderr, "'%s' failed at %s:%u %s()",		\
			#expression , __FILE__, __LINE__, __func__);	\
		pa_context_set_error((context), (error));		\
		return value;						\
	}								\
} while(false)

#define PA_CHECK_VALIDITY_RETURN_NULL(context, expression, error)       \
    PA_CHECK_VALIDITY_RETURN_ANY(context, expression, error, NULL)

#define PA_FAIL(context, error)                                 \
    do {                                                        \
        return -pa_context_set_error((context), (error));          \
    } while(false)

#define PA_FAIL_RETURN_ANY(context, error, value)      \
    do {                                               \
        pa_context_set_error((context), (error));         \
        return value;                                  \
    } while(false)

#define PA_FAIL_RETURN_NULL(context, error)     \
    PA_FAIL_RETURN_ANY(context, error, NULL)

struct pa_proplist {
	struct pw_properties *props;
};

pa_proplist* pa_proplist_new_props(struct pw_properties *props);
pa_proplist* pa_proplist_new_dict(struct spa_dict *dict);

struct pa_io_event {
	struct spa_source *source;
	struct pa_mainloop *mainloop;
	int fd;
	pa_io_event_flags_t events;
	pa_io_event_cb_t cb;
	void *userdata;
	pa_io_event_destroy_cb_t destroy;
};

struct pa_time_event {
	struct spa_source *source;
	struct pa_mainloop *mainloop;
	pa_time_event_cb_t cb;
	void *userdata;
	pa_time_event_destroy_cb_t destroy;
};

struct pa_defer_event {
	struct spa_source *source;
	struct pa_mainloop *mainloop;
	pa_defer_event_cb_t cb;
	void *userdata;
	pa_defer_event_destroy_cb_t destroy;
};

struct pa_mainloop {
	struct pw_loop *loop;
	struct spa_source *event;

	pa_mainloop_api api;

	bool quit;
	int retval;

	int timeout;
	int n_events;
};

struct global {
	struct spa_list link;
	uint32_t id;
	uint32_t parent_id;
	uint32_t type;
	struct pw_properties *props;

	void *info;
	pw_destroy_t destroy;

	struct pw_proxy *proxy;
	struct spa_hook proxy_listener;
        struct spa_hook proxy_proxy_listener;
};

struct pa_context {
	int refcount;

	struct pw_loop *loop;
	struct pw_core *core;
	struct pw_type *t;
	struct pw_remote *remote;
	struct spa_hook remote_listener;

        struct pw_core_proxy *core_proxy;

        struct pw_registry_proxy *registry_proxy;
        struct spa_hook registry_listener;

	pa_proplist *proplist;
	pa_mainloop_api *mainloop;

	uint32_t seq;

	int error;
	pa_context_state_t state;

	pa_context_notify_cb_t state_callback;
	void *state_userdata;
	pa_context_event_cb_t event_callback;
	void *event_userdata;
	pa_context_subscribe_cb_t subscribe_callback;
	void *subscribe_userdata;
	pa_subscription_mask_t subscribe_mask;

	bool no_fail;
	uint32_t client_index;

	struct spa_list globals;

	struct spa_list streams;
	struct spa_list operations;
};

struct global *pa_context_find_global(pa_context *c, uint32_t id);

struct type {
        struct spa_type_media_type media_type;
        struct spa_type_media_subtype media_subtype;
        struct spa_type_format_audio format_audio;
        struct spa_type_audio_format audio_format;
};

static inline void init_type(struct type *type, struct spa_type_map *map)
{
	spa_type_media_type_map(map, &type->media_type);
	spa_type_media_subtype_map(map, &type->media_subtype);
	spa_type_format_audio_map(map, &type->format_audio);
	spa_type_audio_format_map(map, &type->audio_format);
}

#define MAX_BUFFERS     64
#define MASK_BUFFERS    (MAX_BUFFERS-1)

struct pa_stream {
	struct spa_list link;
	int refcount;

	struct pw_stream *stream;
	struct spa_hook stream_listener;

	struct type type;

	pa_context *context;
	pa_proplist *proplist;

	pa_stream_direction_t direction;
	pa_stream_state_t state;
	pa_stream_flags_t flags;
	bool disconnecting;

	pa_sample_spec sample_spec;
	pa_channel_map channel_map;
	uint8_t n_formats;
	pa_format_info *req_formats[PA_MAX_FORMATS];
	pa_format_info *format;

	uint32_t stream_index;

	pa_buffer_attr buffer_attr;

	uint32_t device_index;
	char *device_name;

	pa_timing_info timing_info;

	uint32_t direct_on_input;

	bool suspended:1;
	bool corked:1;
	bool timing_info_valid:1;

	pa_stream_notify_cb_t state_callback;
	void *state_userdata;
	pa_stream_request_cb_t read_callback;
	void *read_userdata;
	pa_stream_request_cb_t write_callback;
	void *write_userdata;
	pa_stream_notify_cb_t overflow_callback;
	void *overflow_userdata;
	pa_stream_notify_cb_t underflow_callback;
	void *underflow_userdata;
	pa_stream_notify_cb_t latency_update_callback;
	void *latency_update_userdata;
	pa_stream_notify_cb_t moved_callback;
	void *moved_userdata;
	pa_stream_notify_cb_t suspended_callback;
	void *suspended_userdata;
	pa_stream_notify_cb_t started_callback;
	void *started_userdata;
	pa_stream_event_cb_t event_callback;
	void *event_userdata;
	pa_stream_notify_cb_t buffer_attr_callback;
	void *buffer_attr_userdata;

	int64_t offset;

	struct pw_buffer *dequeued[MAX_BUFFERS];
	struct spa_ringbuffer dequeued_ring;
	size_t dequeued_size;
	struct spa_list pending;

	struct pw_buffer *buffer;
	uint32_t buffer_index;
	void *buffer_data;
	uint32_t buffer_size;
	uint32_t buffer_offset;
};

void pa_stream_set_state(pa_stream *s, pa_stream_state_t st);

typedef void (*pa_operation_cb_t)(pa_operation *o, void *userdata);

struct pa_operation
{
	struct spa_list link;

	int refcount;
	pa_context *context;
	pa_stream *stream;

	uint32_t seq;
	pa_operation_state_t state;

	pa_operation_cb_t callback;
	void *userdata;

	pa_operation_notify_cb_t state_callback;
	void *state_userdata;
};


pa_operation *pa_operation_new(pa_context *c, pa_stream *s, pa_operation_cb_t cb, size_t userdata_size);
void pa_operation_done(pa_operation *o);

#ifdef __cplusplus
}
#endif

#endif /* __PIPEWIRE_PULSEAUDIO_INTERNAL_H__ */
