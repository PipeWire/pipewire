/* Spa sndio Device */
/* SPDX-FileCopyrightText: Copyright © 2026 Antoine Jacoutot <ajacoutot@openbsd.org> */
/* SPDX-License-Identifier: MIT */

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <poll.h>

#include <sndio.h>

#include <spa/support/plugin.h>
#include <spa/support/log.h>
#include <spa/support/loop.h>
#include <spa/utils/keys.h>
#include <spa/utils/names.h>
#include <spa/utils/result.h>
#include <spa/utils/string.h>
#include <spa/utils/type.h>
#include <spa/node/node.h>
#include <spa/node/keys.h>
#include <spa/pod/builder.h>
#include <spa/pod/filter.h>
#include <spa/pod/parser.h>
#include <spa/param/param.h>
#include <spa/param/audio/raw.h>
#include <spa/monitor/device.h>
#include <spa/monitor/utils.h>

#include "sndio-utils.h"

#undef SPA_LOG_TOPIC_DEFAULT
#define SPA_LOG_TOPIC_DEFAULT &log_topic
SPA_LOG_TOPIC_DEFINE_STATIC(log_topic, "spa.sndio.device");

#define MAX_CHANNELS	8

struct control {
	unsigned int addr;
	unsigned int maxval;
	unsigned int val;
};

#define MAX_CHOICES	8

struct dev_choice {
	unsigned int addr;
	char name[16];
	char display[SIOCTL_DISPLAYMAX];
	unsigned int val;
};

struct dir_controls {
	struct control level[MAX_CHANNELS];
	struct control mute[MAX_CHANNELS];
	uint32_t n_level;
	uint32_t n_mute;
	/* emulated mute when the hardware has no mute switch */
	unsigned int soft_muted:1;
	unsigned int saved_val[MAX_CHANNELS];
};

struct impl {
	struct spa_handle handle;
	struct spa_device device;

	struct spa_log *log;
	struct spa_loop *main_loop;
	struct spa_loop_utils *loop_utils;

	struct spa_hook_list hooks;

	struct spa_device_info info;
	uint32_t info_all;
#define IDX_EnumProfile	0
#define IDX_Profile	1
#define IDX_EnumRoute	2
#define IDX_Route	3
#define N_PARAMS	4
	struct spa_param_info params[N_PARAMS];
	struct spa_dict_item dev_items[2];
	struct spa_dict dev_dict;

	char path[256];
	char buffer_msec[16];

	struct sioctl_hdl *hdl;
	struct spa_source *source;
	struct spa_source *retry_timer;
	uint32_t retry_sec;
	unsigned int ctls_ready:1;

	struct dir_controls out;
	struct dir_controls in;
	struct dev_choice choice[MAX_CHOICES];
	uint32_t n_choice;
	bool route_save[2];	/* WirePlumber persistence flag, per direction */

	uint32_t profile;		/* 0 = off, 1 = on */
	unsigned int emitted_objects:1;
};

/*
 * The desktop exposes volumes as linear amplitude which UIs map
 * cubically; sndio control values are knob positions. Map so that
 * the knob position matches the slider position, which is also what
 * PulseAudio does for mixers without dB information.
 */
static float ctl_to_linear(const struct control *c)
{
	float pos;

	if (c->maxval == 0)
		return 1.0f;
	pos = (float)c->val / (float)c->maxval;
	return pos * pos * pos;
}

static unsigned int linear_to_ctl(const struct control *c, float linear)
{
	float pos = cbrtf(SPA_CLAMPF(linear, 0.0f, 1.0f));
	return (unsigned int)lroundf(pos * (float)c->maxval);
}

static struct dir_controls *get_dir(struct impl *this, enum spa_direction direction)
{
	return direction == SPA_DIRECTION_OUTPUT ? &this->out : &this->in;
}

static uint32_t n_output_routes(struct impl *this)
{
	return this->n_choice > 0 ? this->n_choice : 1;
}

static uint32_t active_choice(struct impl *this)
{
	uint32_t i;
	for (i = 0; i < this->n_choice; i++)
		if (this->choice[i].val)
			return i;
	return 0;
}

static bool dir_has_volume(struct dir_controls *d)
{
	return d->n_level > 0;
}

static bool dir_get_mute(struct dir_controls *d)
{
	if (d->n_mute > 0)
		return d->mute[0].val != 0;
	return d->soft_muted;
}

static void dir_get_volumes(struct dir_controls *d, uint32_t n_channels, float *vols)
{
	uint32_t i;

	for (i = 0; i < n_channels; i++) {
		const struct control *c;
		if (d->n_level == 0) {
			vols[i] = 1.0f;
			continue;
		}
		c = &d->level[SPA_MIN(i, d->n_level - 1)];
		if (d->soft_muted && d->n_mute == 0)
			vols[i] = ctl_to_linear(&(struct control) {
					.val = d->saved_val[SPA_MIN(i, d->n_level - 1)],
					.maxval = c->maxval });
		else
			vols[i] = ctl_to_linear(c);
	}
}

static void dir_set_volumes(struct impl *this, struct dir_controls *d,
		uint32_t n_vols, const float *vols)
{
	uint32_t i;

	for (i = 0; i < d->n_level; i++) {
		struct control *c = &d->level[i];
		float v;
		unsigned int val;

		if (d->n_level == 1 && n_vols > 1) {
			/* one shared knob: use the channel average */
			float sum = 0.0f;
			uint32_t j;
			for (j = 0; j < n_vols; j++)
				sum += cbrtf(SPA_CLAMPF(vols[j], 0.0f, 1.0f));
			val = (unsigned int)lroundf(sum / n_vols * c->maxval);
		} else {
			v = vols[SPA_MIN(i, n_vols - 1)];
			val = linear_to_ctl(c, v);
		}
		if (d->soft_muted && d->n_mute == 0) {
			/* muted in software: update the saved level only */
			d->saved_val[i] = val;
			continue;
		}
		if (val != c->val) {
			c->val = val;
			if (this->hdl != NULL)
				sioctl_setval(this->hdl, c->addr, val);
		}
	}
}

static void dir_set_mute(struct impl *this, struct dir_controls *d, bool mute)
{
	uint32_t i;

	if (d->n_mute > 0) {
		for (i = 0; i < d->n_mute; i++) {
			unsigned int val = mute ? 1 : 0;
			if (val != d->mute[i].val) {
				d->mute[i].val = val;
				if (this->hdl != NULL)
					sioctl_setval(this->hdl, d->mute[i].addr, val);
			}
		}
		return;
	}
	/* no hardware mute switch: emulate with the level controls */
	if (mute && !d->soft_muted) {
		for (i = 0; i < d->n_level; i++) {
			d->saved_val[i] = d->level[i].val;
			d->level[i].val = 0;
			if (this->hdl != NULL)
				sioctl_setval(this->hdl, d->level[i].addr, 0);
		}
		d->soft_muted = true;
	} else if (!mute && d->soft_muted) {
		d->soft_muted = false;
		for (i = 0; i < d->n_level; i++) {
			d->level[i].val = d->saved_val[i];
			if (this->hdl != NULL)
				sioctl_setval(this->hdl, d->level[i].addr,
						d->saved_val[i]);
		}
	}
}

static struct spa_pod *build_route(struct impl *this, struct spa_pod_builder *b,
		uint32_t id, uint32_t index, enum spa_direction direction)
{
	struct dir_controls *d = get_dir(this, direction);
	struct spa_pod_frame f[2];
	float vols[2];
	char name[64], desc[64];
	uint32_t dev = direction == SPA_DIRECTION_OUTPUT ? 0 : 1;
	uint32_t avail = SPA_PARAM_AVAILABILITY_yes;

	if (direction == SPA_DIRECTION_OUTPUT && this->n_choice > 0) {
		struct dev_choice *c = &this->choice[index];
		snprintf(name, sizeof(name), "sndio-output-%s", c->name);
		if (c->display[0] != '\0')
			snprintf(desc, sizeof(desc), "%s", c->display);
		else
			snprintf(desc, sizeof(desc), "Device %s", c->name);
		/* unconfigured slots have no name; hide them from the
		 * desktop until sndiod names them or they get selected */
		if (c->display[0] == '\0' && !c->val)
			avail = SPA_PARAM_AVAILABILITY_no;
	} else if (direction == SPA_DIRECTION_OUTPUT) {
		snprintf(name, sizeof(name), "sndio-output");
		snprintf(desc, sizeof(desc), "Output");
	} else {
		snprintf(name, sizeof(name), "sndio-input");
		snprintf(desc, sizeof(desc), "Input");
	}

	spa_pod_builder_push_object(b, &f[0], SPA_TYPE_OBJECT_ParamRoute, id);
	spa_pod_builder_add(b,
		SPA_PARAM_ROUTE_index, SPA_POD_Int(index),
		SPA_PARAM_ROUTE_direction, SPA_POD_Id(direction),
		SPA_PARAM_ROUTE_name, SPA_POD_String(name),
		SPA_PARAM_ROUTE_description, SPA_POD_String(desc),
		SPA_PARAM_ROUTE_priority, SPA_POD_Int(direction == SPA_DIRECTION_OUTPUT ?
			(int)(n_output_routes(this) - index) : 0),
		SPA_PARAM_ROUTE_available, SPA_POD_Id(avail),
		0);
	if (id == SPA_PARAM_Route)
		spa_pod_builder_add(b,
			SPA_PARAM_ROUTE_device, SPA_POD_Int(dev),
			SPA_PARAM_ROUTE_save, SPA_POD_Bool(this->route_save[dev]),
			0);
	spa_pod_builder_prop(b, SPA_PARAM_ROUTE_profiles, 0);
	spa_pod_builder_array(b, sizeof(uint32_t), SPA_TYPE_Int, 1, (uint32_t[]) { 1 });
	spa_pod_builder_prop(b, SPA_PARAM_ROUTE_devices, 0);
	spa_pod_builder_array(b, sizeof(uint32_t), SPA_TYPE_Int, 1, &dev);

	if (id == SPA_PARAM_Route && dir_has_volume(d)) {
		dir_get_volumes(d, 2, vols);
		spa_pod_builder_prop(b, SPA_PARAM_ROUTE_props, 0);
		spa_pod_builder_push_object(b, &f[1], SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
		spa_pod_builder_add(b,
			SPA_PROP_mute, SPA_POD_Bool(dir_get_mute(d)),
			0);
		spa_pod_builder_prop(b, SPA_PROP_channelVolumes, 0);
		spa_pod_builder_array(b, sizeof(float), SPA_TYPE_Float, 2, vols);
		spa_pod_builder_pop(b, &f[1]);
	}
	return spa_pod_builder_pop(b, &f[0]);
}

static struct spa_pod *build_profile(struct impl *this, struct spa_pod_builder *b,
		uint32_t id, uint32_t index)
{
	struct spa_pod_frame f;

	spa_pod_builder_push_object(b, &f, SPA_TYPE_OBJECT_ParamProfile, id);
	spa_pod_builder_add(b,
		SPA_PARAM_PROFILE_index, SPA_POD_Int(index),
		SPA_PARAM_PROFILE_name, SPA_POD_String(index == 0 ? "off" : "on"),
		SPA_PARAM_PROFILE_description, SPA_POD_String(index == 0 ?
			"Off" : "On"),
		SPA_PARAM_PROFILE_priority, SPA_POD_Int(index == 0 ? 0 : 10),
		SPA_PARAM_PROFILE_available, SPA_POD_Id(SPA_PARAM_AVAILABILITY_yes),
		0);
	if (index == 1) {
		/* the media classes and their device ids; the session
		 * manager needs these to know which devices to select
		 * and restore routes for */
		static const uint32_t playback[1] = { 0 };
		static const uint32_t capture[1] = { 1 };
		struct spa_pod_frame f1;

		spa_pod_builder_prop(b, SPA_PARAM_PROFILE_classes, 0);
		spa_pod_builder_push_struct(b, &f1);
		spa_pod_builder_int(b, 2);
		spa_pod_builder_add_struct(b,
			SPA_POD_String("Audio/Sink"),
			SPA_POD_Int(1),
			SPA_POD_String("card.profile.devices"),
			SPA_POD_Array(sizeof(uint32_t), SPA_TYPE_Int,
				1, playback));
		spa_pod_builder_add_struct(b,
			SPA_POD_String("Audio/Source"),
			SPA_POD_Int(1),
			SPA_POD_String("card.profile.devices"),
			SPA_POD_Array(sizeof(uint32_t), SPA_TYPE_Int,
				1, capture));
		spa_pod_builder_pop(b, &f1);
	}
	return spa_pod_builder_pop(b, &f);
}

static void emit_object_info(struct impl *this, uint32_t id, bool sink)
{
	struct spa_device_object_info info;
	char dev[16];
	struct spa_dict_item items[12];
	uint32_t n_items = 0;

	info = SPA_DEVICE_OBJECT_INFO_INIT();
	info.type = SPA_TYPE_INTERFACE_Node;
	info.factory_name = sink ? SPA_NAME_API_SNDIO_PCM_SINK : SPA_NAME_API_SNDIO_PCM_SOURCE;
	info.change_mask = SPA_DEVICE_OBJECT_CHANGE_MASK_PROPS;

	snprintf(dev, sizeof(dev), "%u", id);
	items[n_items++] = SPA_DICT_ITEM_INIT("node.adapter", SPA_NAME_AUDIO_ADAPT);
	items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_NODE_NAME,
			sink ? "sndio-sink" : "sndio-source");
	items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_NODE_DESCRIPTION,
			sink ? "sndio sink" : "sndio source");
	items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_MEDIA_CLASS,
			sink ? "Audio/Sink" : "Audio/Source");
	items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_DEVICE_API, "sndio");
	items[n_items++] = SPA_DICT_ITEM_INIT("card.profile.device", dev);
	items[n_items++] = SPA_DICT_ITEM_INIT("priority.driver", sink ? "1000" : "900");
	items[n_items++] = SPA_DICT_ITEM_INIT("priority.session", sink ? "1000" : "900");
	items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_AUDIO_POSITION, "FL,FR");
	if (this->path[0] != '\0')
		items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_API_SNDIO_PATH, this->path);
	if (this->buffer_msec[0] != '\0')
		items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_API_SNDIO_BUFFER_MSEC,
				this->buffer_msec);
	info.props = &SPA_DICT_INIT(items, n_items);

	spa_device_emit_object_info(&this->hooks, id, &info);
}

static void emit_objects(struct impl *this, bool emit)
{
	if (emit && !this->emitted_objects) {
		emit_object_info(this, 0, true);
		emit_object_info(this, 1, false);
		this->emitted_objects = true;
	} else if (!emit && this->emitted_objects) {
		spa_device_emit_object_info(&this->hooks, 0, NULL);
		spa_device_emit_object_info(&this->hooks, 1, NULL);
		this->emitted_objects = false;
	}
}

static void emit_route_changed(struct impl *this)
{
	this->info.change_mask = SPA_DEVICE_CHANGE_MASK_PARAMS;
	this->params[IDX_Route].flags ^= SPA_PARAM_INFO_SERIAL;
	spa_device_emit_info(&this->hooks, &this->info);
	this->info.change_mask = 0;
}

static void emit_routes_changed(struct impl *this)
{
	this->params[IDX_EnumRoute].flags ^= SPA_PARAM_INFO_SERIAL;
	emit_route_changed(this);
}

static void schedule_retry(struct impl *this);

/*
 * sioctl callbacks: build the control table from the description
 * events and track value changes made by other programs, such as
 * sndioctl or the desktop volume keys.
 */
static bool remove_addr(struct control *tab, uint32_t *n, unsigned int addr)
{
	uint32_t i;
	for (i = 0; i < *n; i++) {
		if (tab[i].addr == addr) {
			memmove(&tab[i], &tab[i + 1], (*n - i - 1) * sizeof(*tab));
			(*n)--;
			return true;
		}
	}
	return false;
}

static struct control *upsert(struct control *tab, uint32_t *n, unsigned int addr)
{
	uint32_t i;
	for (i = 0; i < *n; i++)
		if (tab[i].addr == addr)
			return &tab[i];
	if (*n < MAX_CHANNELS)
		return &tab[(*n)++];
	return NULL;
}

static void on_desc(void *arg, struct sioctl_desc *desc, int val)
{
	struct impl *this = arg;
	struct dir_controls *d;
	struct control *c = NULL;
	bool changed = false;
	uint32_t i;

	if (desc == NULL) {
		this->ctls_ready = true;
		return;
	}

	if (desc->type == SIOCTL_NONE) {
		/* control removed, e.g. when switching devices */
		changed |= remove_addr(this->out.level, &this->out.n_level, desc->addr);
		changed |= remove_addr(this->out.mute, &this->out.n_mute, desc->addr);
		changed |= remove_addr(this->in.level, &this->in.n_level, desc->addr);
		changed |= remove_addr(this->in.mute, &this->in.n_mute, desc->addr);
		for (i = 0; i < this->n_choice; i++) {
			if (this->choice[i].addr == desc->addr) {
				memmove(&this->choice[i], &this->choice[i + 1],
					(this->n_choice - i - 1) * sizeof(this->choice[0]));
				this->n_choice--;
				changed = true;
				break;
			}
		}
		if (changed && this->ctls_ready)
			emit_routes_changed(this);
		return;
	}
	if (desc->group[0] != '\0')
		return;

	if (spa_streq(desc->node0.name, "server") &&
	    spa_streq(desc->func, "device") && desc->type == SIOCTL_SEL) {
		struct dev_choice *ch = NULL;
		for (i = 0; i < this->n_choice; i++)
			if (this->choice[i].addr == desc->addr)
				ch = &this->choice[i];
		if (ch == NULL && this->n_choice < MAX_CHOICES)
			ch = &this->choice[this->n_choice++];
		if (ch == NULL)
			return;
		ch->addr = desc->addr;
		snprintf(ch->name, sizeof(ch->name), "%s", desc->node1.name);
		snprintf(ch->display, sizeof(ch->display), "%s", desc->display);
		ch->val = val;
		if (this->ctls_ready)
			emit_routes_changed(this);
		spa_log_info(this->log, "%p: device choice %s '%s' addr:%u val:%d",
				this, ch->name, ch->display, desc->addr, val);
		return;
	}

	if (spa_streq(desc->node0.name, "output"))
		d = &this->out;
	else if (spa_streq(desc->node0.name, "input"))
		d = &this->in;
	else
		return;

	if (spa_streq(desc->func, "level") && desc->type == SIOCTL_NUM)
		c = upsert(d->level, &d->n_level, desc->addr);
	else if (spa_streq(desc->func, "mute") && desc->type == SIOCTL_SW)
		c = upsert(d->mute, &d->n_mute, desc->addr);
	if (c == NULL)
		return;

	c->addr = desc->addr;
	c->maxval = desc->maxval;
	c->val = val;
	if (this->ctls_ready)
		emit_routes_changed(this);

	spa_log_info(this->log, "%p: control %s[%d].%s addr:%u max:%u val:%d",
			this, desc->node0.name, desc->node0.unit, desc->func,
			desc->addr, desc->maxval, val);
}

static void on_val(void *arg, unsigned int addr, unsigned int val)
{
	struct impl *this = arg;
	struct dir_controls *dirs[2] = { &this->out, &this->in };
	uint32_t i, j;
	bool changed = false;

	spa_log_debug(this->log, "%p: control value addr:%u val:%u", this, addr, val);

	for (i = 0; i < this->n_choice; i++) {
		if (this->choice[i].addr == addr && this->choice[i].val != val) {
			this->choice[i].val = val;
			changed = true;
		}
	}
	for (i = 0; i < 2; i++) {
		struct dir_controls *d = dirs[i];
		for (j = 0; j < d->n_level; j++) {
			if (d->level[j].addr == addr && d->level[j].val != val) {
				d->level[j].val = val;
				if (d->soft_muted && d->n_mute == 0 && val != 0) {
					/* someone raised the level: unmute */
					d->soft_muted = false;
					d->saved_val[j] = val;
				}
				changed = true;
			}
		}
		for (j = 0; j < d->n_mute; j++) {
			if (d->mute[j].addr == addr && d->mute[j].val != val) {
				d->mute[j].val = val;
				changed = true;
			}
		}
	}
	if (changed && this->ctls_ready)
		emit_route_changed(this);
}

static void on_ctl_io(void *data, int fd, uint32_t mask)
{
	struct impl *this = data;
	struct pollfd pfd;
	int nfds, revents;

	spa_log_debug(this->log, "%p: control io mask:%08x", this, mask);
	spa_sndio_block_sigpipe();

	nfds = sioctl_pollfd(this->hdl, &pfd, POLLIN);
	if (nfds > 0) {
		pfd.revents = POLLIN;
		revents = sioctl_revents(this->hdl, &pfd);
		if (revents & POLLHUP) {
			spa_log_warn(this->log, "%p: sndio control connection lost, "
					"serving cached volumes until it returns", this);
			spa_loop_utils_destroy_source(this->loop_utils, this->source);
			this->source = NULL;
			sioctl_close(this->hdl);
			this->hdl = NULL;
			schedule_retry(this);
		}
	}
}

static int open_controls(struct impl *this, bool reconnect);

static void on_retry(void *data, uint64_t expirations)
{
	struct impl *this = data;
	float out_vols[2], in_vols[2];
	bool had_out, had_in, out_mute, in_mute;
	struct dir_controls save_out, save_in;
	struct dev_choice save_choice[MAX_CHOICES];
	bool was_ready;

	uint32_t prev_active = active_choice(this);
	uint32_t prev_n_choice = this->n_choice;

	had_out = dir_has_volume(&this->out);
	had_in = dir_has_volume(&this->in);
	dir_get_volumes(&this->out, 2, out_vols);
	dir_get_volumes(&this->in, 2, in_vols);
	out_mute = dir_get_mute(&this->out);
	in_mute = dir_get_mute(&this->in);

	save_out = this->out;
	save_in = this->in;
	memcpy(save_choice, this->choice, sizeof(save_choice));
	was_ready = this->ctls_ready;

	spa_zero(this->out);
	spa_zero(this->in);
	spa_zero(this->choice);
	this->n_choice = 0;
	this->ctls_ready = false;
	open_controls(this, true);
	if (this->hdl == NULL) {
		/* still down: keep serving the cached state */
		this->out = save_out;
		this->in = save_in;
		memcpy(this->choice, save_choice, sizeof(save_choice));
		this->n_choice = prev_n_choice;
		this->ctls_ready = was_ready;
		return;
	}

	spa_log_info(this->log, "%p: sndio control device reconnected", this);
	this->params[IDX_EnumRoute].flags ^= SPA_PARAM_INFO_SERIAL;
	if (prev_n_choice > 0 && prev_active < this->n_choice &&
	    prev_active != active_choice(this))
		sioctl_setval(this->hdl, this->choice[prev_active].addr, 1);
	if (had_out && dir_has_volume(&this->out)) {
		dir_set_volumes(this, &this->out, 2, out_vols);
		dir_set_mute(this, &this->out, out_mute);
	}
	if (had_in && dir_has_volume(&this->in)) {
		dir_set_volumes(this, &this->in, 2, in_vols);
		dir_set_mute(this, &this->in, in_mute);
	}
	emit_route_changed(this);
}

static void schedule_retry(struct impl *this)
{
	struct timespec ts;

	if (this->retry_timer == NULL)
		this->retry_timer = spa_loop_utils_add_timer(this->loop_utils,
				on_retry, this);
	if (this->retry_timer == NULL)
		return;
	if (this->retry_sec == 0)
		this->retry_sec = 1;
	ts.tv_sec = this->retry_sec;
	ts.tv_nsec = 0;
	spa_loop_utils_update_timer(this->loop_utils, this->retry_timer,
			&ts, NULL, false);
	this->retry_sec = SPA_MIN(this->retry_sec * 2, 5u);
}

static int open_controls(struct impl *this, bool reconnect)
{
	struct pollfd pfd;
	char name[256];
	int nfds;

	spa_sndio_block_sigpipe();
	/* the first open may land on the raw mixer when no sndiod runs,
	 * like the streams; reconnects insist on sndiod */
	if (reconnect)
		spa_sndio_reconnect_name(this->path, NULL, name, sizeof(name));
	else
		spa_scnprintf(name, sizeof(name), "%s",
				this->path[0] != '\0' ? this->path : SIO_DEVANY);
	this->hdl = sioctl_open(name, SIOCTL_READ | SIOCTL_WRITE, 0);
	if (this->hdl == NULL) {
		spa_log_debug(this->log, "%p: no sndio control device, "
				"volume handled in software until one appears", this);
		schedule_retry(this);
		return 0;
	}
	this->retry_sec = 1;
	if (!sioctl_ondesc(this->hdl, on_desc, this)) {
		spa_log_warn(this->log, "%p: cannot get control descriptions", this);
		sioctl_close(this->hdl);
		this->hdl = NULL;
		schedule_retry(this);
		return 0;
	}
	sioctl_onval(this->hdl, on_val, this);

	nfds = sioctl_pollfd(this->hdl, &pfd, POLLIN);
	if (nfds > 0) {
		this->source = spa_loop_utils_add_io(this->loop_utils, pfd.fd,
				SPA_IO_IN, false, on_ctl_io, this);
	}

	spa_log_info(this->log, "%p: controls ready: output level:%u mute:%u, "
			"input level:%u mute:%u", this,
			this->out.n_level, this->out.n_mute,
			this->in.n_level, this->in.n_mute);
	return 0;
}

static int impl_enum_params(void *object, int seq,
		uint32_t id, uint32_t start, uint32_t num,
		const struct spa_pod *filter)
{
	struct impl *this = object;
	struct spa_pod_builder b = { 0 };
	uint8_t buffer[2048];
	struct spa_result_device_params result;
	struct spa_pod *param;
	uint32_t count = 0;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(num != 0, -EINVAL);

	result.id = id;
	result.next = start;
next:
	result.index = result.next++;

	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	switch (id) {
	case SPA_PARAM_EnumProfile:
		if (result.index > 1)
			return 0;
		param = build_profile(this, &b, id, result.index);
		break;
	case SPA_PARAM_Profile:
		if (result.index > 0)
			return 0;
		param = build_profile(this, &b, id, this->profile);
		break;
	case SPA_PARAM_EnumRoute:
	{
		uint32_t n_out = n_output_routes(this);
		if (result.index < n_out) {
			if (this->n_choice == 0 && !dir_has_volume(&this->out))
				goto next;
			param = build_route(this, &b, id, result.index,
					SPA_DIRECTION_OUTPUT);
		} else if (result.index == n_out) {
			if (!dir_has_volume(&this->in))
				goto next;
			param = build_route(this, &b, id, result.index,
					SPA_DIRECTION_INPUT);
		} else
			return 0;
		break;
	}
	case SPA_PARAM_Route:
	{
		uint32_t n_out = n_output_routes(this);
		if (this->profile == 0)
			return 0;
		/* the active route per direction */
		if (result.index == 0) {
			if (this->n_choice == 0 && !dir_has_volume(&this->out))
				goto next;
			param = build_route(this, &b, id, active_choice(this),
					SPA_DIRECTION_OUTPUT);
		} else if (result.index == 1) {
			if (!dir_has_volume(&this->in))
				goto next;
			param = build_route(this, &b, id, n_out,
					SPA_DIRECTION_INPUT);
		} else
			return 0;
		break;
	}
	default:
		return -ENOENT;
	}

	if (spa_pod_filter(&b, &result.param, param, filter) < 0)
		goto next;

	spa_device_emit_result(&this->hooks, seq, 0,
			SPA_RESULT_TYPE_DEVICE_PARAMS, &result);

	if (++count != num)
		goto next;

	return 0;
}

static int apply_route_props(struct impl *this, struct dir_controls *d,
		const struct spa_pod *props)
{
	struct spa_pod_prop *prop;
	struct spa_pod_object *obj = (struct spa_pod_object *) props;
	float vols[MAX_CHANNELS];
	uint32_t n_vols;
	bool mute;

	SPA_POD_OBJECT_FOREACH(obj, prop) {
		switch (prop->key) {
		case SPA_PROP_mute:
			if (spa_pod_get_bool(&prop->value, &mute) == 0)
				dir_set_mute(this, d, mute);
			break;
		case SPA_PROP_channelVolumes:
			n_vols = spa_pod_copy_array(&prop->value, SPA_TYPE_Float,
					vols, MAX_CHANNELS);
			if (n_vols > 0)
				dir_set_volumes(this, d, n_vols, vols);
			break;
		default:
			break;
		}
	}
	return 0;
}

static int impl_set_param(void *object,
		uint32_t id, uint32_t flags, const struct spa_pod *param)
{
	struct impl *this = object;
	int res;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_sndio_block_sigpipe();

	switch (id) {
	case SPA_PARAM_Profile:
	{
		uint32_t index;
		if ((res = spa_pod_parse_object(param,
				SPA_TYPE_OBJECT_ParamProfile, NULL,
				SPA_PARAM_PROFILE_index, SPA_POD_Int(&index))) < 0)
			return res;
		if (index > 1)
			return -EINVAL;
		if (index != this->profile) {
			this->profile = index;
			emit_objects(this, index == 1);
			this->info.change_mask = SPA_DEVICE_CHANGE_MASK_PARAMS;
			this->params[IDX_Profile].flags ^= SPA_PARAM_INFO_SERIAL;
			this->params[IDX_EnumRoute].flags ^= SPA_PARAM_INFO_SERIAL;
			this->params[IDX_Route].flags ^= SPA_PARAM_INFO_SERIAL;
			spa_device_emit_info(&this->hooks, &this->info);
			this->info.change_mask = 0;
		}
		break;
	}
	case SPA_PARAM_Route:
	{
		uint32_t index, n_out = n_output_routes(this);
		struct spa_pod *props = NULL;
		struct dir_controls *d;
		bool save = false;

		if ((res = spa_pod_parse_object(param,
				SPA_TYPE_OBJECT_ParamRoute, NULL,
				SPA_PARAM_ROUTE_index, SPA_POD_Int(&index),
				SPA_PARAM_ROUTE_save, SPA_POD_OPT_Bool(&save),
				SPA_PARAM_ROUTE_props, SPA_POD_OPT_Pod(&props))) < 0)
			return res;
		if (index < n_out) {
			d = &this->out;
			if (this->n_choice > 0 && index != active_choice(this)) {
				/* select another sndiod device */
				uint32_t i;
				for (i = 0; i < this->n_choice; i++)
					this->choice[i].val = i == index;
				if (this->hdl != NULL)
					sioctl_setval(this->hdl,
						this->choice[index].addr, 1);
				spa_log_info(this->log, "%p: selected device %s",
						this, this->choice[index].name);
			}
		} else if (index == n_out) {
			d = &this->in;
		} else
			return -EINVAL;
		if (save)
			this->route_save[index < n_out ? 0 : 1] = true;
		spa_log_debug(this->log, "%p: route set index:%u save:%d props:%d",
				this, index, save, props != NULL);
		if (props != NULL) {
			if (!dir_has_volume(d))
				return -ENOTSUP;
			apply_route_props(this, d, props);
		}
		emit_route_changed(this);
		break;
	}
	default:
		return -ENOENT;
	}
	return 0;
}

static int impl_add_listener(void *object,
		struct spa_hook *listener,
		const struct spa_device_events *events, void *data)
{
	struct impl *this = object;
	struct spa_hook_list save;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(events != NULL, -EINVAL);

	spa_hook_list_isolate(&this->hooks, &save, listener, events, data);

	this->info.change_mask = this->info_all;
	spa_device_emit_info(&this->hooks, &this->info);
	this->info.change_mask = 0;

	if (this->profile == 1) {
		if (this->emitted_objects) {
			emit_object_info(this, 0, true);
			emit_object_info(this, 1, false);
		} else {
			emit_objects(this, true);
		}
	}

	spa_hook_list_join(&this->hooks, &save);
	return 0;
}

static int impl_sync(void *object, int seq)
{
	struct impl *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_device_emit_result(&this->hooks, seq, 0, 0, NULL);
	return 0;
}

static const struct spa_device_methods impl_device = {
	SPA_VERSION_DEVICE_METHODS,
	.add_listener = impl_add_listener,
	.sync = impl_sync,
	.enum_params = impl_enum_params,
	.set_param = impl_set_param,
};

static int impl_get_interface(struct spa_handle *handle, const char *type, void **interface)
{
	struct impl *this;

	spa_return_val_if_fail(handle != NULL, -EINVAL);
	spa_return_val_if_fail(interface != NULL, -EINVAL);

	this = (struct impl *) handle;

	if (spa_streq(type, SPA_TYPE_INTERFACE_Device))
		*interface = &this->device;
	else
		return -ENOENT;
	return 0;
}

static int impl_clear(struct spa_handle *handle)
{
	struct impl *this = (struct impl *) handle;

	spa_sndio_block_sigpipe();
	if (this->retry_timer != NULL)
		spa_loop_utils_destroy_source(this->loop_utils, this->retry_timer);
	if (this->source != NULL)
		spa_loop_utils_destroy_source(this->loop_utils, this->source);
	if (this->hdl != NULL)
		sioctl_close(this->hdl);
	return 0;
}

static size_t impl_get_size(const struct spa_handle_factory *factory,
		const struct spa_dict *params)
{
	return sizeof(struct impl);
}

static int impl_init(const struct spa_handle_factory *factory,
		struct spa_handle *handle,
		const struct spa_dict *info,
		const struct spa_support *support,
		uint32_t n_support)
{
	struct impl *this;
	const char *str;

	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(handle != NULL, -EINVAL);

	handle->get_interface = impl_get_interface;
	handle->clear = impl_clear;

	this = (struct impl *) handle;

	this->log = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_Log);
	spa_log_topic_init(this->log, &log_topic);
	this->main_loop = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_Loop);
	this->loop_utils = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_LoopUtils);

	if (this->main_loop == NULL || this->loop_utils == NULL) {
		spa_log_error(this->log, "a main-loop and loop-utils are needed");
		return -EINVAL;
	}

	this->device.iface = SPA_INTERFACE_INIT(
			SPA_TYPE_INTERFACE_Device,
			SPA_VERSION_DEVICE,
			&impl_device, this);
	spa_hook_list_init(&this->hooks);

	if (info && (str = spa_dict_lookup(info, SPA_KEY_API_SNDIO_PATH)) != NULL)
		snprintf(this->path, sizeof(this->path), "%s", str);
	if (info && (str = spa_dict_lookup(info, SPA_KEY_API_SNDIO_BUFFER_MSEC)) != NULL)
		snprintf(this->buffer_msec, sizeof(this->buffer_msec), "%s", str);

	this->info = SPA_DEVICE_INFO_INIT();
	this->info_all = SPA_DEVICE_CHANGE_MASK_PROPS |
			SPA_DEVICE_CHANGE_MASK_PARAMS;
	this->dev_items[0] = SPA_DICT_ITEM_INIT(SPA_KEY_DEVICE_API, "sndio");
	this->dev_items[1] = SPA_DICT_ITEM_INIT(SPA_KEY_MEDIA_CLASS, "Audio/Device");
	this->dev_dict = SPA_DICT_INIT(this->dev_items, 2);
	this->info.props = &this->dev_dict;
	this->params[IDX_EnumProfile] = SPA_PARAM_INFO(SPA_PARAM_EnumProfile, SPA_PARAM_INFO_READ);
	this->params[IDX_Profile] = SPA_PARAM_INFO(SPA_PARAM_Profile, SPA_PARAM_INFO_READWRITE);
	this->params[IDX_EnumRoute] = SPA_PARAM_INFO(SPA_PARAM_EnumRoute, SPA_PARAM_INFO_READ);
	this->params[IDX_Route] = SPA_PARAM_INFO(SPA_PARAM_Route, SPA_PARAM_INFO_READWRITE);
	this->info.params = this->params;
	this->info.n_params = N_PARAMS;

	this->profile = 1;

	open_controls(this, false);

	spa_log_info(this->log, "%p: initialized", this);
	return 0;
}

static const struct spa_interface_info impl_interfaces[] = {
	{ SPA_TYPE_INTERFACE_Device },
};

static int impl_enum_interface_info(const struct spa_handle_factory *factory,
		const struct spa_interface_info **info, uint32_t *index)
{
	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(info != NULL, -EINVAL);
	spa_return_val_if_fail(index != NULL, -EINVAL);

	switch (*index) {
	case 0:
		*info = &impl_interfaces[*index];
		break;
	default:
		return 0;
	}
	(*index)++;
	return 1;
}

static const struct spa_dict_item info_items[] = {
	{ SPA_KEY_FACTORY_AUTHOR, "Antoine Jacoutot <ajacoutot@openbsd.org>" },
	{ SPA_KEY_FACTORY_DESCRIPTION, "A sndio device and its volume controls" },
};

static const struct spa_dict info = SPA_DICT_INIT_ARRAY(info_items);

const struct spa_handle_factory spa_sndio_device_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	SPA_NAME_API_SNDIO_DEVICE,
	&info,
	impl_get_size,
	impl_init,
	impl_enum_interface_info,
};
