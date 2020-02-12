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
#include <spa/param/audio/format-utils.h>

#include <pulse/stream.h>
#include <pulse/format.h>
#include <pulse/subscribe.h>
#include <pulse/introspect.h>
#include <pulse/version.h>

#include <pipewire/pipewire.h>

/* Some PulseAudio API added const qualifiers in 13.0 */
#if PA_MAJOR >= 13
#define PA_CONST const
#else
#define PA_CONST
#endif

#define PA_MAX_FORMATS (PA_ENCODING_MAX)

#ifdef __cplusplus
extern "C" {
#endif

#define pa_streq(a,b)		(!strcmp((a),(b)))
#define pa_strneq(a,b,n)	(!strncmp((a),(b),(n)))

#ifndef PA_LIKELY
#define PA_UNLIKELY		SPA_UNLIKELY
#define PA_LIKELY		SPA_LIKELY
#endif
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

#ifdef __GNUC__
#define PA_ROUND_DOWN(a, b)				\
	__extension__ ({				\
		typeof(a) _a = (a);			\
		typeof(b) _b = (b);			\
		(_a / _b) * _b;				\
	})
#else
#define PA_ROUND_DOWN(a, b) (((a) / (b)) * (b))
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

int pa_context_set_error(PA_CONST pa_context *c, int error);
void pa_context_fail(PA_CONST pa_context *c, int error);

#define PA_CHECK_VALIDITY(context, expression, error)			\
do {									\
	if (!(expression)) {						\
		pw_log_trace("'%s' failed at %s:%u %s()",		\
			#expression, __FILE__, __LINE__, __func__);	\
		return -pa_context_set_error((context), (error));	\
	}								\
} while(false)

#define PA_CHECK_VALIDITY_RETURN_ANY(context, expression, error, value)	\
do {									\
	if (!(expression)) {						\
		pw_log_trace("'%s' failed at %s:%u %s()",		\
			#expression, __FILE__, __LINE__, __func__);	\
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
int pa_proplist_update_dict(pa_proplist *p, struct spa_dict *dict);

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

struct param {
	struct spa_list link;
	uint32_t id;
	int seq;
	void *param;
};

#define PA_IDX_FLAG_DSP		0x800000U
#define PA_IDX_MASK_DSP		0x7fffffU

struct global {
	struct spa_list link;
	uint32_t id;
	char *type;
	struct pw_properties *props;

	pa_context *context;
	pa_subscription_mask_t mask;
	pa_subscription_event_type_t event;

	int priority_master;
	int pending_seq;
	int init:1;
	int subscribed:1;

	void *info;
	pw_destroy_t destroy;

	struct pw_proxy *proxy;
	struct spa_hook proxy_listener;
        struct spa_hook object_listener;

	union {
		/* for links */
		struct {
			struct global *src;
			struct global *dst;
		} link_info;
		/* for sink/source */
		struct {
			uint32_t client_id;
			uint32_t monitor;
			float volume;
			bool mute;
			uint32_t n_channel_volumes;
			float channel_volumes[SPA_AUDIO_MAX_CHANNELS];
			uint32_t device_id;
		} node_info;
		struct {
			uint32_t node_id;
		} port_info;
		/* for devices */
		struct {
			struct spa_list profiles;
			uint32_t n_profiles;
			uint32_t active_profile;
			pa_card_info info;
		} card_info;
		struct {
			pa_module_info info;
		} module_info;
		struct {
			pa_client_info info;
		} client_info;
	};
};

struct pa_context {
	int refcount;
	uint32_t client_index;

	struct pw_loop *loop;
	struct pw_context *context;

	struct pw_properties *props;

	struct pw_core *core;
	struct spa_hook core_listener;
	struct pw_core_info *core_info;

        struct pw_registry *registry;
        struct spa_hook registry_listener;

	pa_proplist *proplist;
	pa_mainloop_api *mainloop;

	int error;
	pa_context_state_t state;

	pa_context_notify_cb_t state_callback;
	void *state_userdata;
	pa_context_event_cb_t event_callback;
	void *event_userdata;
	pa_context_subscribe_cb_t subscribe_callback;
	void *subscribe_userdata;
	pa_subscription_mask_t subscribe_mask;

	struct spa_list globals;

	struct spa_list streams;
	struct spa_list operations;

	int no_fail:1;
	int disconnect:1;
};

struct global *pa_context_find_global(pa_context *c, uint32_t id);
struct global *pa_context_find_global_by_name(pa_context *c, uint32_t mask, const char *name);
struct global *pa_context_find_linked(pa_context *c, uint32_t id);

#define MAX_BUFFERS     64u
#define MASK_BUFFERS    (MAX_BUFFERS-1)

struct pa_stream {
	struct spa_list link;
	int refcount;

	struct pw_stream *stream;
	struct spa_hook stream_listener;

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
	size_t maxsize;
	struct spa_list pending;

	struct pw_buffer *buffer;
	uint32_t buffer_index;
	void *buffer_data;
	uint32_t buffer_size;
	uint32_t buffer_offset;

	uint32_t n_channel_volumes;
	float channel_volumes[SPA_AUDIO_MAX_CHANNELS];
	bool mute;
	pa_operation *drain;
	uint64_t queued;
};

void pa_stream_set_state(pa_stream *s, pa_stream_state_t st);

typedef void (*pa_operation_cb_t)(pa_operation *o, void *userdata);

struct pa_operation
{
	struct spa_list link;

	int refcount;
	pa_context *context;
	pa_stream *stream;

	int seq;
	pa_operation_state_t state;

	pa_operation_cb_t callback;
	void *userdata;

	pa_operation_notify_cb_t state_callback;
	void *state_userdata;
};


pa_operation *pa_operation_new(pa_context *c, pa_stream *s, pa_operation_cb_t cb, size_t userdata_size);
void pa_operation_done(pa_operation *o);
int pa_operation_sync(pa_operation *o);

#ifdef __cplusplus
}
#endif

#endif /* __PIPEWIRE_PULSEAUDIO_INTERNAL_H__ */
