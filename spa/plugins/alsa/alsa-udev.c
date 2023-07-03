/* Spa ALSA udev */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <stddef.h>
#include <stdio.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/inotify.h>
#include <fcntl.h>
#include <dirent.h>

#include <libudev.h>
#include <alsa/asoundlib.h>

#include <spa/utils/cleanup.h>
#include <spa/utils/type.h>
#include <spa/utils/keys.h>
#include <spa/utils/names.h>
#include <spa/utils/result.h>
#include <spa/utils/string.h>
#include <spa/support/loop.h>
#include <spa/support/plugin.h>
#include <spa/monitor/device.h>
#include <spa/monitor/utils.h>

#include "alsa.h"

#define MAX_DEVICES	64

#define ACTION_ADD	0
#define ACTION_REMOVE	1
#define ACTION_DISABLE	2

struct device {
	uint32_t id;
	struct udev_device *dev;
	unsigned int unavailable:1;
	unsigned int accessible:1;
	unsigned int ignored:1;
	unsigned int emitted:1;
};

struct impl {
	struct spa_handle handle;
	struct spa_device device;

	struct spa_log *log;
	struct spa_loop *main_loop;
	struct spa_system *main_system;

	struct spa_hook_list hooks;

	uint64_t info_all;
	struct spa_device_info info;

	struct udev *udev;
	struct udev_monitor *umonitor;

	struct device devices[MAX_DEVICES];
        uint32_t n_devices;

	struct spa_source source;
	struct spa_source notify;
	unsigned int use_acp:1;
};

static int impl_udev_open(struct impl *this)
{
	if (this->udev == NULL) {
		this->udev = udev_new();
		if (this->udev == NULL)
			return -ENOMEM;
	}
	return 0;
}

static int impl_udev_close(struct impl *this)
{
	if (this->udev != NULL)
		udev_unref(this->udev);
	this->udev = NULL;
	return 0;
}

static struct device *add_device(struct impl *this, uint32_t id, struct udev_device *dev)
{
	struct device *device;

	if (this->n_devices >= MAX_DEVICES)
		return NULL;
	device = &this->devices[this->n_devices++];
	spa_zero(*device);
	device->id = id;
	udev_device_ref(dev);
	device->dev = dev;
	return device;
}

static struct device *find_device(struct impl *this, uint32_t id)
{
	uint32_t i;
	for (i = 0; i < this->n_devices; i++) {
		if (this->devices[i].id == id)
			return &this->devices[i];
	}
	return NULL;
}

static void remove_device(struct impl *this, struct device *device)
{
	udev_device_unref(device->dev);
	*device = this->devices[--this->n_devices];
}

static void clear_devices(struct impl *this)
{
        uint32_t i;
	for (i = 0; i < this->n_devices; i++)
	        udev_device_unref(this->devices[i].dev);
	this->n_devices = 0;
}

static uint32_t get_card_id(struct impl *this, struct udev_device *dev)
{
	const char *e, *str;

	if (udev_device_get_property_value(dev, "ACP_IGNORE"))
		return SPA_ID_INVALID;

	if ((str = udev_device_get_property_value(dev, "SOUND_CLASS")) && spa_streq(str, "modem"))
		return SPA_ID_INVALID;

	if (udev_device_get_property_value(dev, "SOUND_INITIALIZED") == NULL)
		return SPA_ID_INVALID;

	if ((str = udev_device_get_property_value(dev, "DEVPATH")) == NULL)
		return SPA_ID_INVALID;

	if ((e = strrchr(str, '/')) == NULL)
		return SPA_ID_INVALID;

	if (strlen(e) <= 5 || strncmp(e, "/card", 5) != 0)
		return SPA_ID_INVALID;

	return atoi(e + 5);
}

static int dehex(char x)
{
	if (x >= '0' && x <= '9')
		return x - '0';
	if (x >= 'A' && x <= 'F')
		return x - 'A' + 10;
	if (x >= 'a' && x <= 'f')
		return x - 'a' + 10;
	return -1;
}

static void unescape(const char *src, char *dst)
{
	const char *s;
	char *d;
	int h1 = 0, h2 = 0;
	enum { TEXT, BACKSLASH, EX, FIRST } state = TEXT;

	for (s = src, d = dst; *s; s++) {
		switch (state) {
		case TEXT:
			if (*s == '\\')
				state = BACKSLASH;
			else
				*(d++) = *s;
			break;

		case BACKSLASH:
			if (*s == 'x')
				state = EX;
			else {
				*(d++) = '\\';
				*(d++) = *s;
				state = TEXT;
			}
			break;

		case EX:
			h1 = dehex(*s);
			if (h1 < 0) {
				*(d++) = '\\';
				*(d++) = 'x';
				*(d++) = *s;
				state = TEXT;
			} else
				state = FIRST;
			break;

		case FIRST:
			h2 = dehex(*s);
			if (h2 < 0) {
				*(d++) = '\\';
				*(d++) = 'x';
				*(d++) = *(s-1);
				*(d++) = *s;
			} else
				*(d++) = (char) (h1 << 4) | h2;
			state = TEXT;
			break;
		}
	}
	switch (state) {
	case TEXT:
		break;
	case BACKSLASH:
		*(d++) = '\\';
		break;
	case EX:
		*(d++) = '\\';
		*(d++) = 'x';
		break;
	case FIRST:
		*(d++) = '\\';
		*(d++) = 'x';
		*(d++) = *(s-1);
		break;
	}
	*d = 0;
}

static int check_device_pcm_class(const char *devname)
{
	char path[PATH_MAX];
	char buf[16];
	size_t sz;

	/* Check device class */
	spa_scnprintf(path, sizeof(path), "/sys/class/sound/%s/pcm_class", devname);

	spa_autoptr(FILE) f = fopen(path, "re");
	if (f == NULL)
		return -errno;
	sz = fread(buf, 1, sizeof(buf) - 1, f);
	buf[sz] = '\0';
	return spa_strstartswith(buf, "modem") ? -ENXIO : 0;
}

static int get_num_pcm_devices(unsigned int card_id)
{
	char prefix[32];
	struct dirent *entry;
	int num_dev = 0;
	int res;

	/* Check if card has PCM devices, without opening them */

	spa_scnprintf(prefix, sizeof(prefix), "pcmC%uD", card_id);

	spa_autoptr(DIR) snd = opendir("/dev/snd");
	if (snd == NULL)
		return -errno;

	while ((errno = 0, entry = readdir(snd)) != NULL) {
		if (!(entry->d_type == DT_CHR &&
				spa_strstartswith(entry->d_name, prefix)))
			continue;

		res = check_device_pcm_class(entry->d_name);
		if (res != -ENXIO) {
			/* count device also if sysfs status file not accessible */
			++num_dev;
		}
	}

	return errno != 0 ? -errno : num_dev;
}

static int check_device_available(struct impl *this, struct device *device, int *num_pcm)
{
	char path[PATH_MAX];
	char buf[16];
	size_t sz;
	struct dirent *entry, *entry_pcm;
	int res;

	res = get_num_pcm_devices(device->id);
	if (res < 0) {
		spa_log_error(this->log, "Error finding PCM devices for ALSA card %u: %s",
			(unsigned int)device->id, spa_strerror(res));
		return res;
	}
	*num_pcm = res;

	spa_log_debug(this->log, "card %u has %d pcm device(s)", (unsigned int)device->id, *num_pcm);

	/*
	 * Check if some pcm devices of the card are busy.  Check it via /proc, as we
	 * don't want to actually open any devices using alsa-lib (generates uncontrolled
	 * number of inotify events), or replicate its subdevice logic.
	 *
	 * The /proc/asound directory might not exist if kernel is compiled with
	 * CONFIG_SND_PROCFS=n, and the pcmXX directories may be missing if compiled
	 * with CONFIG_SND_VERBOSE_PROCFS=n. In those cases, the busy check always succeeds.
	 */

	res = 0;

	spa_scnprintf(path, sizeof(path), "/proc/asound/card%u", (unsigned int)device->id);

	spa_autoptr(DIR) card = opendir(path);
	if (card == NULL)
		goto done;

	while ((errno = 0, entry = readdir(card)) != NULL) {
		if (!(entry->d_type == DT_DIR &&
				spa_strstartswith(entry->d_name, "pcm")))
			continue;

		spa_scnprintf(path, sizeof(path), "pcmC%uD%s",
				(unsigned int)device->id, entry->d_name+3);
		if (check_device_pcm_class(path) < 0)
			continue;

		/* Check busy status */
		spa_scnprintf(path, sizeof(path), "/proc/asound/card%u/%s",
				(unsigned int)device->id, entry->d_name);

		spa_autoptr(DIR) pcm = opendir(path);
		if (pcm == NULL)
			goto done;

		while ((errno = 0, entry_pcm = readdir(pcm)) != NULL) {
			if (!(entry_pcm->d_type == DT_DIR &&
					spa_strstartswith(entry_pcm->d_name, "sub")))
				continue;

			spa_scnprintf(path, sizeof(path), "/proc/asound/card%u/%s/%s/status",
					(unsigned int)device->id, entry->d_name, entry_pcm->d_name);

			spa_autoptr(FILE) f = fopen(path, "re");
			if (f == NULL)
				goto done;
			sz = fread(buf, 1, 6, f);
			buf[sz] = '\0';

			if (!spa_strstartswith(buf, "closed")) {
				spa_log_debug(this->log, "card %u pcm device %s busy",
						(unsigned int)device->id, entry->d_name);
				res = -EBUSY;
				goto done;
			}
			spa_log_debug(this->log, "card %u pcm device %s free",
					(unsigned int)device->id, entry->d_name);
		}
		if (errno != 0)
			goto done;
	}
	if (errno != 0)
		goto done;

done:
	if (errno != 0) {
		spa_log_info(this->log, "card %u: failed to find busy status (%s)",
				(unsigned int)device->id, spa_strerror(-errno));
	}

	return res;
}

static int emit_object_info(struct impl *this, struct device *device)
{
	struct spa_device_object_info info;
	uint32_t id = device->id;
	struct udev_device *dev = device->dev;
	const char *str;
	char path[32], *cn = NULL, *cln = NULL;
	struct spa_dict_item items[25];
	uint32_t n_items = 0;
	int res, pcm;

	/*
	 * inotify close events under /dev/snd must not be emitted, except after setting
	 * device->emitted to true. alsalib functions can be used after that.
	 */

	snprintf(path, sizeof(path), "hw:%u", id);

	if ((res = check_device_available(this, device, &pcm)) < 0)
		return res;
	if (pcm == 0) {
		spa_log_debug(this->log, "no pcm devices for %s", path);
		device->ignored = true;
		return -ENODEV;
	}

	spa_log_debug(this->log, "emitting card %s", path);
	device->emitted = true;

	info = SPA_DEVICE_OBJECT_INFO_INIT();

	info.type = SPA_TYPE_INTERFACE_Device;
	info.factory_name = this->use_acp ?
		SPA_NAME_API_ALSA_ACP_DEVICE :
		SPA_NAME_API_ALSA_PCM_DEVICE;
	info.change_mask = SPA_DEVICE_OBJECT_CHANGE_MASK_FLAGS |
		SPA_DEVICE_OBJECT_CHANGE_MASK_PROPS;
	info.flags = 0;

	items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_DEVICE_ENUM_API, "udev");
	items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_DEVICE_API,  "alsa");
	items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_MEDIA_CLASS, "Audio/Device");
	items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_API_ALSA_PATH, path);
	items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_API_ALSA_CARD, path+3);
	if (snd_card_get_name(id, &cn) >= 0)
		items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_API_ALSA_CARD_NAME, cn);
	if (snd_card_get_longname(id, &cln) >= 0)
		items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_API_ALSA_CARD_LONGNAME, cln);

	if ((str = udev_device_get_property_value(dev, "ACP_NAME")) && *str)
		items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_DEVICE_NAME, str);

	if ((str = udev_device_get_property_value(dev, "ACP_PROFILE_SET")) && *str)
		items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_DEVICE_PROFILE_SET, str);

	if ((str = udev_device_get_property_value(dev, "SOUND_CLASS")) && *str)
		items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_DEVICE_CLASS, str);

	if ((str = udev_device_get_property_value(dev, "USEC_INITIALIZED")) && *str)
		items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_DEVICE_PLUGGED_USEC, str);

	str = udev_device_get_property_value(dev, "ID_PATH");
	if (!(str && *str))
		str = udev_device_get_syspath(dev);
	if (str && *str) {
		items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_DEVICE_BUS_PATH, str);
	}
	if ((str = udev_device_get_devpath(dev)) && *str) {
		items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_DEVICE_SYSFS_PATH, str);
	}
	if ((str = udev_device_get_property_value(dev, "ID_ID")) && *str) {
		items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_DEVICE_BUS_ID, str);
	}
	if ((str = udev_device_get_property_value(dev, "ID_BUS")) && *str) {
		items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_DEVICE_BUS, str);
	}
	if ((str = udev_device_get_property_value(dev, "SUBSYSTEM")) && *str) {
		items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_DEVICE_SUBSYSTEM, str);
	}
	if ((str = udev_device_get_property_value(dev, "ID_VENDOR_ID")) && *str) {
		int32_t val;
		if (spa_atoi32(str, &val, 16)) {
			char *dec = alloca(12); /* 0xffffffff is max */
			snprintf(dec, 12, "0x%04x", val);
			items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_DEVICE_VENDOR_ID, dec);
		}
	}
	str = udev_device_get_property_value(dev, "ID_VENDOR_FROM_DATABASE");
	if (!(str && *str)) {
		str = udev_device_get_property_value(dev, "ID_VENDOR_ENC");
		if (!(str && *str)) {
			str = udev_device_get_property_value(dev, "ID_VENDOR");
		} else {
			char *t = alloca(strlen(str) + 1);
			unescape(str, t);
			str = t;
		}
	}
	if (str && *str) {
		items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_DEVICE_VENDOR_NAME, str);
	}
	if ((str = udev_device_get_property_value(dev, "ID_MODEL_ID")) && *str) {
		int32_t val;
		if (spa_atoi32(str, &val, 16)) {
			char *dec = alloca(12); /* 0xffffffff is max */
			snprintf(dec, 12, "0x%04x", val);
			items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_DEVICE_PRODUCT_ID, dec);
		}
	}
	str = udev_device_get_property_value(dev, "ID_MODEL_FROM_DATABASE");
	if (!(str && *str)) {
		str = udev_device_get_property_value(dev, "ID_MODEL_ENC");
		if (!(str && *str)) {
			str = udev_device_get_property_value(dev, "ID_MODEL");
		} else {
			char *t = alloca(strlen(str) + 1);
			unescape(str, t);
			str = t;
		}
	}
	if (str && *str)
		items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_DEVICE_PRODUCT_NAME, str);

	if ((str = udev_device_get_property_value(dev, "ID_SERIAL")) && *str) {
		items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_DEVICE_SERIAL, str);
	}
	if ((str = udev_device_get_property_value(dev, "SOUND_FORM_FACTOR")) && *str) {
		items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_DEVICE_FORM_FACTOR, str);
	}
	info.props = &SPA_DICT_INIT(items, n_items);

	spa_device_emit_object_info(&this->hooks, id, &info);
	free(cn);
	free(cln);

	return 1;
}

static bool check_access(struct impl *this, struct device *device)
{
	char path[128], prefix[32];
	spa_autoptr(DIR) snd = NULL;
	struct dirent *entry;
	bool accessible = false;

	snprintf(path, sizeof(path), "/dev/snd/controlC%u", device->id);
	if (access(path, R_OK|W_OK) >= 0 && (snd = opendir("/dev/snd"))) {
		/*
		 * It's possible that controlCX is accessible before pcmCX* or
		 * the other way around. Return true only if all devices are
                 * accessible.
		 */

		accessible = true;
		spa_scnprintf(prefix, sizeof(prefix), "pcmC%uD", device->id);
		while ((entry = readdir(snd)) != NULL) {
			if (!(entry->d_type == DT_CHR &&
					spa_strstartswith(entry->d_name, prefix)))
				continue;

			snprintf(path, sizeof(path), "/dev/snd/%.32s", entry->d_name);
			if (access(path, R_OK|W_OK) < 0) {
				accessible = false;
				break;
			}
		}
	}

	if (accessible != device->accessible)
		spa_log_debug(this->log, "%s accessible:%u", path, accessible);
	device->accessible = accessible;

	return device->accessible;
}

static void process_device(struct impl *this, uint32_t action, struct udev_device *dev)
{
	uint32_t id;
	struct device *device;
	bool emitted;
	int res;

	if ((id = get_card_id(this, dev)) == SPA_ID_INVALID)
		return;

	device = find_device(this, id);
	if (device && device->ignored)
		return;

	switch (action) {
	case ACTION_ADD:
		if (device == NULL)
			device = add_device(this, id, dev);
		if (device == NULL)
			return;
		if (!check_access(this, device))
			return;
		res = emit_object_info(this, device);
		if (res < 0) {
			if (device->ignored)
				spa_log_info(this->log, "ALSA card %u unavailable (%s): it is ignored",
						device->id, spa_strerror(res));
			else if (!device->unavailable)
				spa_log_info(this->log, "ALSA card %u unavailable (%s): wait for it",
						device->id, spa_strerror(res));
			else
				spa_log_debug(this->log, "ALSA card %u still unavailable (%s)",
						device->id, spa_strerror(res));
			device->unavailable = true;
		} else {
			if (device->unavailable)
				spa_log_info(this->log, "ALSA card %u now available",
						device->id);
			device->unavailable = false;
		}
		break;

	case ACTION_REMOVE:
		if (device == NULL)
			return;
		emitted = device->emitted;
		remove_device(this, device);
		if (emitted)
			spa_device_emit_object_info(&this->hooks, id, NULL);
		break;

	case ACTION_DISABLE:
		if (device == NULL)
			return;
		if (device->emitted) {
			device->emitted = false;
			spa_device_emit_object_info(&this->hooks, id, NULL);
		}
		break;
	}
}

static int stop_inotify(struct impl *this)
{
	if (this->notify.fd == -1)
		return 0;
	spa_log_info(this->log, "stop inotify");
	spa_loop_remove_source(this->main_loop, &this->notify);
	close(this->notify.fd);
	this->notify.fd = -1;
	return 0;
}

static void impl_on_notify_events(struct spa_source *source)
{
	bool deleted = false;
	struct impl *this = source->data;
	union {
		struct inotify_event e;
		char name[NAME_MAX+1+sizeof(struct inotify_event)];
	} buf;

	while (true) {
		ssize_t len;
		const struct inotify_event *event;
		void *p, *e;

		len = read(source->fd, &buf, sizeof(buf));
		if (len < 0 && errno != EAGAIN)
			break;
		if (len <= 0)
			break;

		e = SPA_PTROFF(&buf, len, void);

		for (p = &buf; p < e;
		     p = SPA_PTROFF(p, sizeof(struct inotify_event) + event->len, void)) {
			unsigned int id;
			struct device *device;

			event = (const struct inotify_event *) p;
			spa_assert_se(SPA_PTRDIFF(e, p) >= (ptrdiff_t)sizeof(struct inotify_event) &&
			              SPA_PTRDIFF(e, p) - sizeof(struct inotify_event) >= event->len &&
			              "bad event from kernel");

			/* Device becomes accessible or not busy */
			if ((event->mask & (IN_ATTRIB | IN_CLOSE_WRITE))) {
				bool access;
				if (sscanf(event->name, "controlC%u", &id) != 1 &&
				    sscanf(event->name, "pcmC%uD", &id) != 1)
					continue;
				if ((device = find_device(this, id)) == NULL)
					continue;

				access = check_access(this, device);
				if (access && !device->emitted)
					process_device(this, ACTION_ADD, device->dev);
				else if (!access && device->emitted)
					process_device(this, ACTION_DISABLE, device->dev);
			}
			/* /dev/snd/ might have been removed */
			if ((event->mask & (IN_DELETE_SELF | IN_MOVE_SELF)))
				deleted = true;
		}
	}
	if (deleted)
		stop_inotify(this);
}

static int start_inotify(struct impl *this)
{
	int res, notify_fd;

	if (this->notify.fd != -1)
		return 0;

	if ((notify_fd = inotify_init1(IN_CLOEXEC | IN_NONBLOCK)) < 0)
		return -errno;

	res = inotify_add_watch(notify_fd, "/dev/snd",
			IN_ATTRIB | IN_CLOSE_WRITE | IN_DELETE_SELF | IN_MOVE_SELF);
	if (res < 0) {
		res = -errno;
		close(notify_fd);

		if (res == -ENOENT) {
			spa_log_debug(this->log, "/dev/snd/ does not exist yet");
			return 0;
		}
		spa_log_error(this->log, "inotify_add_watch() failed: %s", spa_strerror(res));
		return res;
	}
	spa_log_info(this->log, "start inotify");
	this->notify.func = impl_on_notify_events;
	this->notify.data = this;
	this->notify.fd = notify_fd;
	this->notify.mask = SPA_IO_IN | SPA_IO_ERR;

	spa_loop_add_source(this->main_loop, &this->notify);

	return 0;
}

static void impl_on_fd_events(struct spa_source *source)
{
	struct impl *this = source->data;
	struct udev_device *dev;
	const char *action;

	dev = udev_monitor_receive_device(this->umonitor);
	if (dev == NULL)
		return;

	if ((action = udev_device_get_action(dev)) == NULL)
		action = "change";

	spa_log_debug(this->log, "action %s", action);

	start_inotify(this);

	if (spa_streq(action, "change")) {
		process_device(this, ACTION_ADD, dev);
	} else if (spa_streq(action, "remove")) {
		process_device(this, ACTION_REMOVE, dev);
	}
	udev_device_unref(dev);
}

static int start_monitor(struct impl *this)
{
	int res;

	if (this->umonitor != NULL)
		return 0;

	this->umonitor = udev_monitor_new_from_netlink(this->udev, "udev");
	if (this->umonitor == NULL)
		return -ENOMEM;

	udev_monitor_filter_add_match_subsystem_devtype(this->umonitor,
							"sound", NULL);
	udev_monitor_enable_receiving(this->umonitor);

	this->source.func = impl_on_fd_events;
	this->source.data = this;
	this->source.fd = udev_monitor_get_fd(this->umonitor);
	this->source.mask = SPA_IO_IN | SPA_IO_ERR;

	spa_log_debug(this->log, "monitor %p", this->umonitor);
	spa_loop_add_source(this->main_loop, &this->source);

	if ((res = start_inotify(this)) < 0)
		return res;

	return 0;
}

static int stop_monitor(struct impl *this)
{
	if (this->umonitor == NULL)
		return 0;

        clear_devices (this);

	spa_loop_remove_source(this->main_loop, &this->source);
	udev_monitor_unref(this->umonitor);
	this->umonitor = NULL;

	stop_inotify(this);

	return 0;
}

static int enum_devices(struct impl *this)
{
	struct udev_enumerate *enumerate;
	struct udev_list_entry *devices;

	enumerate = udev_enumerate_new(this->udev);
	if (enumerate == NULL)
		return -ENOMEM;

	udev_enumerate_add_match_subsystem(enumerate, "sound");
	udev_enumerate_scan_devices(enumerate);

	for (devices = udev_enumerate_get_list_entry(enumerate); devices;
			devices = udev_list_entry_get_next(devices)) {
		struct udev_device *dev;

		dev = udev_device_new_from_syspath(this->udev, udev_list_entry_get_name(devices));
		if (dev == NULL)
			continue;

		process_device(this, ACTION_ADD, dev);

		udev_device_unref(dev);
	}
	udev_enumerate_unref(enumerate);

	return 0;
}

static const struct spa_dict_item device_info_items[] = {
	{ SPA_KEY_DEVICE_API, "udev" },
	{ SPA_KEY_DEVICE_NICK, "alsa-udev" },
	{ SPA_KEY_API_UDEV_MATCH, "sound" },
};

static void emit_device_info(struct impl *this, bool full)
{
	uint64_t old = full ? this->info.change_mask : 0;
	if (full)
		this->info.change_mask = this->info_all;
	if (this->info.change_mask) {
		this->info.props = &SPA_DICT_INIT_ARRAY(device_info_items);
		spa_device_emit_info(&this->hooks, &this->info);
		this->info.change_mask = old;
	}
}

static void impl_hook_removed(struct spa_hook *hook)
{
	struct impl *this = hook->priv;
	if (spa_hook_list_is_empty(&this->hooks)) {
		stop_monitor(this);
		impl_udev_close(this);
	}
}

static int
impl_device_add_listener(void *object, struct spa_hook *listener,
		const struct spa_device_events *events, void *data)
{
	int res;
	struct impl *this = object;
        struct spa_hook_list save;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(events != NULL, -EINVAL);

	if ((res = impl_udev_open(this)) < 0)
		return res;

        spa_hook_list_isolate(&this->hooks, &save, listener, events, data);

	emit_device_info(this, true);

	if ((res = start_monitor(this)) < 0)
		return res;

	if ((res = enum_devices(this)) < 0)
		return res;

        spa_hook_list_join(&this->hooks, &save);

	listener->removed = impl_hook_removed;
	listener->priv = this;

	return 0;
}

static const struct spa_device_methods impl_device = {
	SPA_VERSION_DEVICE_METHODS,
	.add_listener = impl_device_add_listener,
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
	stop_monitor(this);
	impl_udev_close(this);
	return 0;
}

static size_t
impl_get_size(const struct spa_handle_factory *factory,
	      const struct spa_dict *params)
{
	return sizeof(struct impl);
}

static int
impl_init(const struct spa_handle_factory *factory,
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
	this->notify.fd = -1;

	this->log = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_Log);
	alsa_log_topic_init(this->log);
	this->main_loop = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_Loop);
	this->main_system = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_System);

	if (this->main_loop == NULL) {
		spa_log_error(this->log, "a main-loop is needed");
		return -EINVAL;
	}
	if (this->main_system == NULL) {
		spa_log_error(this->log, "a main-system is needed");
		return -EINVAL;
	}
	spa_hook_list_init(&this->hooks);

	this->device.iface = SPA_INTERFACE_INIT(
			SPA_TYPE_INTERFACE_Device,
			SPA_VERSION_DEVICE,
			&impl_device, this);

	this->info = SPA_DEVICE_INFO_INIT();
	this->info_all = SPA_DEVICE_CHANGE_MASK_FLAGS |
			SPA_DEVICE_CHANGE_MASK_PROPS;
	this->info.flags = 0;

	if (info) {
		if ((str = spa_dict_lookup(info, "alsa.use-acp")) != NULL)
			this->use_acp = spa_atob(str);
	}

	return 0;
}

static const struct spa_interface_info impl_interfaces[] = {
	{SPA_TYPE_INTERFACE_Device,},
};

static int
impl_enum_interface_info(const struct spa_handle_factory *factory,
			 const struct spa_interface_info **info,
			 uint32_t *index)
{
	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(info != NULL, -EINVAL);
	spa_return_val_if_fail(index != NULL, -EINVAL);

	if (*index >= SPA_N_ELEMENTS(impl_interfaces))
		return 0;

	*info = &impl_interfaces[(*index)++];
	return 1;
}

const struct spa_handle_factory spa_alsa_udev_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	SPA_NAME_API_ALSA_ENUM_UDEV,
	NULL,
	impl_get_size,
	impl_init,
	impl_enum_interface_info,
};
