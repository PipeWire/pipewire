/* Spa */
/* SPDX-FileCopyrightText: Copyright © 2020 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <stddef.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#ifdef __linux__
#include <linux/ethtool.h>
#include <linux/sockios.h>
#endif
#include <net/if.h>

#include <spa/support/plugin.h>
#include <spa/support/log.h>
#include <spa/support/loop.h>
#include <spa/utils/names.h>
#include <spa/utils/result.h>
#include <spa/utils/string.h>
#include <spa/utils/dll.h>
#include <spa/node/node.h>
#include <spa/node/keys.h>
#include <spa/node/io.h>
#include <spa/node/utils.h>
#include <spa/param/param.h>

#define NAME "driver"

#define DEFAULT_FREEWHEEL	false
#define DEFAULT_FREEWHEEL_WAIT	10
#define DEFAULT_CLOCK_PREFIX	"clock.system"
#define DEFAULT_CLOCK_ID	CLOCK_MONOTONIC
#define DEFAULT_RESYNC_MS	10

#define CLOCKFD 3
#define FD_TO_CLOCKID(fd)	((~(clockid_t) (fd) << 3) | CLOCKFD)
#define CLOCKID_TO_FD(clk)	((unsigned int) ~((clk) >> 3))

#define BW_PERIOD	(3 * SPA_NSEC_PER_SEC)
#define MAX_ERROR_MS	1

struct props {
	bool freewheel;
	char clock_name[64];
	clockid_t clock_id;
	uint32_t freewheel_wait;
	float resync_ms;
};

struct impl {
	struct spa_handle handle;
	struct spa_node node;

	struct props props;

	struct spa_log *log;
	struct spa_loop *data_loop;
	struct spa_system *data_system;

	uint64_t info_all;
	struct spa_node_info info;
	struct spa_param_info params[1];

	struct spa_hook_list hooks;
	struct spa_callbacks callbacks;

	struct spa_io_position *position;
	struct spa_io_clock *clock;

	struct spa_source timer_source;
	struct itimerspec timerspec;
	int clock_fd;

	bool started;
	bool following;
	bool tracking;
	clockid_t timer_clockid;
	uint64_t next_time;
	uint64_t last_time;
	uint64_t base_time;
	struct spa_dll dll;
	double max_error;
	double max_resync;
};

static void reset_props(struct props *props)
{
	props->freewheel = DEFAULT_FREEWHEEL;
	spa_zero(props->clock_name);
	props->clock_id = CLOCK_MONOTONIC;
	props->freewheel_wait = DEFAULT_FREEWHEEL_WAIT;
	props->resync_ms = DEFAULT_RESYNC_MS;
}

static const struct clock_info {
	const char *name;
	clockid_t id;
} clock_info[] = {
	{ "realtime", CLOCK_REALTIME },
#ifdef CLOCK_TAI
	{ "tai", CLOCK_TAI },
#endif
	{ "monotonic", CLOCK_MONOTONIC },
#ifdef CLOCK_MONOTONIC_RAW
	{ "monotonic-raw", CLOCK_MONOTONIC_RAW },
#endif
#ifdef CLOCK_BOOTTIME
	{ "boottime", CLOCK_BOOTTIME },
#endif
};

static bool clock_for_timerfd(clockid_t id)
{
	return id == CLOCK_REALTIME ||
#ifdef CLOCK_BOOTTIME
		id == CLOCK_BOOTTIME ||
#endif
		id == CLOCK_MONOTONIC;
}

static clockid_t clock_name_to_id(const char *name)
{
	SPA_FOR_EACH_ELEMENT_VAR(clock_info, i) {
		if (spa_streq(i->name, name))
			return i->id;
	}
	return -1;
}
static const char *clock_id_to_name(clockid_t id)
{
	SPA_FOR_EACH_ELEMENT_VAR(clock_info, i) {
		if (i->id == id)
			return i->name;
	}
	return "custom";
}


static void set_timeout(struct impl *this, uint64_t next_time)
{
	spa_log_trace(this->log, "set timeout %"PRIu64, next_time);
	this->timerspec.it_value.tv_sec = next_time / SPA_NSEC_PER_SEC;
	this->timerspec.it_value.tv_nsec = next_time % SPA_NSEC_PER_SEC;
	spa_system_timerfd_settime(this->data_system,
			this->timer_source.fd, SPA_FD_TIMER_ABSTIME, &this->timerspec, NULL);
}

static inline uint64_t gettime_nsec(struct impl *this, clockid_t clock_id)
{
	struct timespec now = { 0 };
	uint64_t nsec;
	if (spa_system_clock_gettime(this->data_system, clock_id, &now) < 0)
		return 0;
	nsec = SPA_TIMESPEC_TO_NSEC(&now);
	spa_log_trace(this->log, "%p now:%"PRIu64, this, nsec);
	return nsec;
}

static int set_timers(struct impl *this)
{
	this->next_time = gettime_nsec(this, this->timer_clockid);

	spa_log_debug(this->log, "%p now:%"PRIu64, this, this->next_time);

	if (this->following || !this->started) {
		set_timeout(this, 0);
	} else {
		set_timeout(this, this->next_time);
	}
	return 0;
}

static inline bool is_following(struct impl *this)
{
	return this->position && this->clock && this->position->clock.id != this->clock->id;
}

static int do_set_timers(struct spa_loop *loop,
			    bool async,
			    uint32_t seq,
			    const void *data,
			    size_t size,
			    void *user_data)
{
	struct impl *this = user_data;
	set_timers(this);
	return 0;
}

static int reassign_follower(struct impl *this)
{
	bool following;

	if (this->clock)
		SPA_FLAG_UPDATE(this->clock->flags,
				SPA_IO_CLOCK_FLAG_FREEWHEEL, this->props.freewheel);

	if (!this->started)
		return 0;

	following = is_following(this);
	if (following != this->following) {
		spa_log_debug(this->log, NAME" %p: reassign follower %d->%d", this, this->following, following);
		this->following = following;
		spa_loop_invoke(this->data_loop, do_set_timers, 0, NULL, 0, true, this);
	}
	return 0;
}

static int impl_node_set_io(void *object, uint32_t id, void *data, size_t size)
{
	struct impl *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	switch (id) {
	case SPA_IO_Clock:
		if (size > 0 && size < sizeof(struct spa_io_clock))
			return -EINVAL;
		this->clock = data;
		if (this->clock)
			spa_scnprintf(this->clock->name, sizeof(this->clock->name),
					"%s", this->props.clock_name);
		break;
	case SPA_IO_Position:
		if (size > 0 && size < sizeof(struct spa_io_position))
			return -EINVAL;
		this->position = data;
		break;
	default:
		return -ENOENT;
	}
	reassign_follower(this);

	return 0;
}

static inline uint64_t scale_u64(uint64_t val, uint32_t num, uint32_t denom)
{
#if 0
	return ((__uint128_t)val * num) / denom;
#else
	return (double)val / denom * num;
#endif
}

static void on_timeout(struct spa_source *source)
{
	struct impl *this = source->data;
	uint64_t expirations, nsec, duration, current_time, current_position, position;
	uint32_t rate;
	double corr = 1.0, err = 0.0;
	int res;

	if ((res = spa_system_timerfd_read(this->data_system,
				this->timer_source.fd, &expirations)) < 0) {
		if (res != -EAGAIN)
			spa_log_error(this->log, NAME " %p: timerfd error: %s",
					this, spa_strerror(res));
		return;
	}
	if (SPA_LIKELY(this->position)) {
		duration = this->position->clock.target_duration;
		rate = this->position->clock.target_rate.denom;
	} else {
		duration = 1024;
		rate = 48000;
	}
	if (this->props.freewheel)
		nsec = gettime_nsec(this, this->props.clock_id);
	else
		nsec = this->next_time;

	if (this->tracking)
		/* we are actually following another clock */
		current_time = gettime_nsec(this, this->props.clock_id);
	else
		current_time = nsec;

	current_position = scale_u64(current_time, rate, SPA_NSEC_PER_SEC);

	if (this->last_time == 0) {
		spa_dll_set_bw(&this->dll, SPA_DLL_BW_MIN, duration, rate);
		this->max_error = rate * MAX_ERROR_MS / 1000;
		this->max_resync = rate * this->props.resync_ms / 1000;
		position = current_position;
	} else if (SPA_LIKELY(this->clock)) {
		position = this->clock->position + this->clock->duration;
	} else {
		position = current_position;
	}

	this->last_time = current_time;

	if (this->props.freewheel) {
		corr = 1.0;
		this->next_time = nsec + this->props.freewheel_wait * SPA_NSEC_PER_SEC;
	} else if (this->tracking) {
		/* check the elapsed time of the other clock against
		 * the graph clock elapsed time, feed this error into the
		 * dll and adjust the timeout of our MONOTONIC clock. */
		err = (double)position - (double)current_position;
		if (fabs(err) > this->max_error) {
			if (fabs(err) > this->max_resync) {
				spa_log_warn(this->log, "err %f > max_resync %f, resetting",
						err, this->max_resync);
				spa_dll_set_bw(&this->dll, SPA_DLL_BW_MIN, duration, rate);
				position = current_position;
				err = 0.0;
			} else {
				err = SPA_CLAMPD(err, -this->max_error, this->max_error);
			}
		}
		corr = spa_dll_update(&this->dll, err);
		this->next_time = nsec + duration / corr * 1e9 / rate;
	} else {
		corr = 1.0;
		this->next_time = scale_u64(position + duration, SPA_NSEC_PER_SEC, rate);
	}

	if (SPA_UNLIKELY((this->next_time - this->base_time) > BW_PERIOD)) {
		this->base_time = this->next_time;
		spa_log_debug(this->log, "%p: rate:%f "
			"bw:%f dur:%"PRIu64" max:%f drift:%f",
				this, corr, this->dll.bw, duration,
				this->max_error, err);
	}

	if (SPA_LIKELY(this->clock)) {
		this->clock->nsec = nsec;
		this->clock->rate = this->clock->target_rate;
		this->clock->position = position;
		this->clock->duration = duration;
		this->clock->delay = 0;
		this->clock->rate_diff = corr;
		this->clock->next_nsec = this->next_time;
	}

	spa_node_call_ready(&this->callbacks,
			SPA_STATUS_HAVE_DATA | SPA_STATUS_NEED_DATA);

	set_timeout(this, this->next_time);
}

static int do_start(struct impl *this)
{
	if (this->started)
		return 0;

	this->following = is_following(this);
	this->started = true;
	this->last_time = 0;
	spa_loop_invoke(this->data_loop, do_set_timers, 0, NULL, 0, true, this);
	return 0;
}

static int do_stop(struct impl *this)
{
	if (!this->started)
		return 0;
	this->started = false;
	spa_loop_invoke(this->data_loop, do_set_timers, 0, NULL, 0, true, this);
	return 0;
}

static int impl_node_send_command(void *object, const struct spa_command *command)
{
	struct impl *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(command != NULL, -EINVAL);


	switch (SPA_NODE_COMMAND_ID(command)) {
	case SPA_NODE_COMMAND_Start:
		do_start(this);
		break;
	case SPA_NODE_COMMAND_Suspend:
	case SPA_NODE_COMMAND_Pause:
		do_stop(this);
		break;
	default:
		return -ENOTSUP;
	}
	return 0;
}

static void emit_node_info(struct impl *this, bool full)
{
	uint64_t old = full ? this->info.change_mask : 0;
	if (full)
		this->info.change_mask = this->info_all;
	if (this->info.change_mask) {
		struct spa_dict_item items[3];

		items[0] = SPA_DICT_ITEM_INIT(SPA_KEY_NODE_DRIVER, "true");
		items[1] = SPA_DICT_ITEM_INIT("clock.id", clock_id_to_name(this->props.clock_id));
		items[2] = SPA_DICT_ITEM_INIT("clock.name", this->props.clock_name);

		this->info.props = &SPA_DICT_INIT(items, 3);
		spa_node_emit_info(&this->hooks, &this->info);
		this->info.change_mask = old;
	}
}

static int impl_node_add_listener(void *object,
		struct spa_hook *listener,
		const struct spa_node_events *events,
		void *data)
{
	struct impl *this = object;
	struct spa_hook_list save;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	spa_hook_list_isolate(&this->hooks, &save, listener, events, data);

	emit_node_info(this, true);

	spa_hook_list_join(&this->hooks, &save);

	return 0;
}

static int
impl_node_set_callbacks(void *object,
			const struct spa_node_callbacks *callbacks,
			void *data)
{
	struct impl *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	this->callbacks = SPA_CALLBACKS_INIT(callbacks, data);

	return 0;
}

static int impl_node_process(void *object)
{
	struct impl *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_log_trace(this->log, "process %d", this->props.freewheel);

	if (this->props.freewheel) {
		this->next_time = gettime_nsec(this, this->timer_clockid);
		set_timeout(this, this->next_time);
	}
	return SPA_STATUS_HAVE_DATA | SPA_STATUS_NEED_DATA;
}

static const struct spa_node_methods impl_node = {
	SPA_VERSION_NODE_METHODS,
	.add_listener = impl_node_add_listener,
	.set_callbacks = impl_node_set_callbacks,
	.set_io = impl_node_set_io,
	.send_command = impl_node_send_command,
	.process = impl_node_process,
};

static int impl_get_interface(struct spa_handle *handle, const char *type, void **interface)
{
	struct impl *this;

	spa_return_val_if_fail(handle != NULL, -EINVAL);
	spa_return_val_if_fail(interface != NULL, -EINVAL);

	this = (struct impl *) handle;

	if (spa_streq(type, SPA_TYPE_INTERFACE_Node))
		*interface = &this->node;
	else
		return -ENOENT;

	return 0;
}

static int do_remove_timer(struct spa_loop *loop, bool async, uint32_t seq, const void *data, size_t size, void *user_data)
{
	struct impl *this = user_data;
	spa_loop_remove_source(this->data_loop, &this->timer_source);
	return 0;
}

static int impl_clear(struct spa_handle *handle)
{
	struct impl *this;

	spa_return_val_if_fail(handle != NULL, -EINVAL);

	this = (struct impl *) handle;

	spa_loop_invoke(this->data_loop, do_remove_timer, 0, NULL, 0, true, this);
	spa_system_close(this->data_system, this->timer_source.fd);

	if (this->clock_fd != -1)
		close(this->clock_fd);

	return 0;
}

static size_t
impl_get_size(const struct spa_handle_factory *factory,
	      const struct spa_dict *params)
{
	return sizeof(struct impl);
}

int get_phc_index(struct spa_system *s, const char *name) {
#ifdef ETHTOOL_GET_TS_INFO
	struct ethtool_ts_info info = {0};
	struct ifreq ifr = {0};
	int fd, err;

	info.cmd = ETHTOOL_GET_TS_INFO;
	strncpy(ifr.ifr_name, name, IFNAMSIZ - 1);
	ifr.ifr_data = (char *) &info;

	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0)
		return -errno;

	err = spa_system_ioctl(s, fd, SIOCETHTOOL, &ifr);
	close(fd);
	if (err < 0)
		return -errno;

	return info.phc_index;
#else
	return -ENOTSUP;
#endif
}

static int
impl_init(const struct spa_handle_factory *factory,
	  struct spa_handle *handle,
	  const struct spa_dict *info,
	  const struct spa_support *support,
	  uint32_t n_support)
{
	struct impl *this;
	uint32_t i;

	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(handle != NULL, -EINVAL);

	handle->get_interface = impl_get_interface;
	handle->clear = impl_clear;

	this = (struct impl *) handle;

	this->log = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_Log);
	this->data_loop = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_DataLoop);
	this->data_system = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_DataSystem);
	this->clock_fd = -1;
	spa_dll_init(&this->dll);

	if (this->data_loop == NULL) {
		spa_log_error(this->log, "a data_loop is needed");
		return -EINVAL;
	}
	if (this->data_system == NULL) {
		spa_log_error(this->log, "a data_system is needed");
		return -EINVAL;
	}

	spa_hook_list_init(&this->hooks);

	this->node.iface = SPA_INTERFACE_INIT(
			SPA_TYPE_INTERFACE_Node,
			SPA_VERSION_NODE,
			&impl_node, this);

	this->info_all = SPA_NODE_CHANGE_MASK_FLAGS |
			SPA_NODE_CHANGE_MASK_PROPS |
			SPA_NODE_CHANGE_MASK_PARAMS;
	this->info = SPA_NODE_INFO_INIT();
	this->info.max_input_ports = 0;
	this->info.max_output_ports = 0;
	this->info.flags = SPA_NODE_FLAG_RT;
	this->params[0] = SPA_PARAM_INFO(SPA_PARAM_Props, SPA_PARAM_INFO_READWRITE);
	this->info.params = this->params;
	this->info.n_params = 0;

	reset_props(&this->props);

	for (i = 0; info && i < info->n_items; i++) {
		const char *k = info->items[i].key;
		const char *s = info->items[i].value;
		if (spa_streq(k, "node.freewheel")) {
			this->props.freewheel = spa_atob(s);
		} else if (spa_streq(k, "clock.name") && this->clock_fd < 0) {
			spa_scnprintf(this->props.clock_name,
				sizeof(this->props.clock_name), "%s", s);
		} else if (spa_streq(k, "clock.id") && this->clock_fd < 0) {
			this->props.clock_id = clock_name_to_id(s);
			if (this->props.clock_id == -1) {
				spa_log_warn(this->log, "unknown clock id '%s'", s);
				this->props.clock_id = DEFAULT_CLOCK_ID;
			}
		} else if (spa_streq(k, "clock.device")) {
			if (this->clock_fd >= 0) {
				close(this->clock_fd);
			}
			this->clock_fd = open(s, O_RDWR);

			if (this->clock_fd == -1) {
				spa_log_warn(this->log, "failed to open clock device '%s': %m", s);
			} else {
				this->props.clock_id = FD_TO_CLOCKID(this->clock_fd);
			}
		} else if (spa_streq(k, "clock.interface") && this->clock_fd < 0) {
			int phc_index = get_phc_index(this->data_system, s);
			if (phc_index < 0) {
				spa_log_warn(this->log, "failed to get phc device index for interface '%s': %s",
						s, spa_strerror(phc_index));
			} else {
				char dev[19];
				spa_scnprintf(dev, sizeof(dev), "/dev/ptp%d", phc_index);
				this->clock_fd = open(dev, O_RDONLY);
				if (this->clock_fd == -1) {
					spa_log_warn(this->log, "failed to open clock device '%s' "
							"for interface '%s': %m", dev, s);
				} else {
					this->props.clock_id = FD_TO_CLOCKID(this->clock_fd);
				}
			}
		} else if (spa_streq(k, "freewheel.wait")) {
			this->props.freewheel_wait = atoi(s);
		} else if (spa_streq(k, "resync.ms")) {
			this->props.resync_ms = atof(s);
		}
	}
	if (this->props.clock_name[0] == '\0') {
		spa_scnprintf(this->props.clock_name, sizeof(this->props.clock_name),
				"%s.%s", DEFAULT_CLOCK_PREFIX,
				clock_id_to_name(this->props.clock_id));
	}

	this->tracking = !clock_for_timerfd(this->props.clock_id);
	this->timer_clockid = this->tracking ? CLOCK_MONOTONIC : this->props.clock_id;
	this->max_error = 128;

	this->timer_source.func = on_timeout;
	this->timer_source.data = this;
	this->timer_source.fd = spa_system_timerfd_create(this->data_system,
			this->timer_clockid, SPA_FD_CLOEXEC | SPA_FD_NONBLOCK);

	this->timer_source.mask = SPA_IO_IN;
	this->timer_source.rmask = 0;
	this->timerspec.it_value.tv_sec = 0;
	this->timerspec.it_value.tv_nsec = 0;
	this->timerspec.it_interval.tv_sec = 0;
	this->timerspec.it_interval.tv_nsec = 0;

	spa_loop_add_source(this->data_loop, &this->timer_source);

	return 0;
}

static const struct spa_interface_info impl_interfaces[] = {
	{SPA_TYPE_INTERFACE_Node,},
};

static int
impl_enum_interface_info(const struct spa_handle_factory *factory,
			 const struct spa_interface_info **info,
			 uint32_t *index)
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

const struct spa_handle_factory spa_support_node_driver_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	SPA_NAME_SUPPORT_NODE_DRIVER,
	NULL,
	impl_get_size,
	impl_init,
	impl_enum_interface_info,
};
