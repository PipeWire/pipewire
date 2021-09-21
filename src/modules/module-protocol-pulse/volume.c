/* PipeWire
 *
 * Copyright © 2020 Wim Taymans
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <spa/param/props.h>
#include <spa/param/audio/raw.h>
#include <spa/pod/iter.h>
#include <spa/utils/defs.h>
#include <pipewire/log.h>

#include "log.h"
#include "volume.h"

int volume_compare(struct volume *vol, struct volume *other)
{
	uint8_t i;
	if (vol->channels != other->channels) {
		pw_log_info("channels %d<>%d", vol->channels, other->channels);
		return -1;
	}
	for (i = 0; i < vol->channels; i++) {
		if (vol->values[i] != other->values[i]) {
			pw_log_info("%d: val %f<>%f", i, vol->values[i], other->values[i]);
			return -1;
		}
	}
	return 0;
}

int volume_parse_param(const struct spa_pod *param, struct volume_info *info, bool monitor)
{
	struct spa_pod_object *obj = (struct spa_pod_object *) param;
	struct spa_pod_prop *prop;

	SPA_POD_OBJECT_FOREACH(obj, prop) {
		switch (prop->key) {
		case SPA_PROP_volume:
			if (spa_pod_get_float(&prop->value, &info->level) < 0)
				continue;
			SPA_FLAG_UPDATE(info->flags, VOLUME_HW_VOLUME,
					prop->flags & SPA_POD_PROP_FLAG_HARDWARE);

			break;
		case SPA_PROP_mute:
			if (monitor)
				continue;
			if (spa_pod_get_bool(&prop->value, &info->mute) < 0)
				continue;
			SPA_FLAG_UPDATE(info->flags, VOLUME_HW_MUTE,
					prop->flags & SPA_POD_PROP_FLAG_HARDWARE);
			break;
		case SPA_PROP_channelVolumes:
			if (monitor)
				continue;
			info->volume.channels = spa_pod_copy_array(&prop->value, SPA_TYPE_Float,
					info->volume.values, SPA_AUDIO_MAX_CHANNELS);
			SPA_FLAG_UPDATE(info->flags, VOLUME_HW_VOLUME,
					prop->flags & SPA_POD_PROP_FLAG_HARDWARE);
			break;
		case SPA_PROP_monitorMute:
			if (!monitor)
				continue;
			if (spa_pod_get_bool(&prop->value, &info->mute) < 0)
				continue;
			SPA_FLAG_CLEAR(info->flags, VOLUME_HW_MUTE);
			break;
		case SPA_PROP_monitorVolumes:
			if (!monitor)
				continue;
			info->volume.channels = spa_pod_copy_array(&prop->value, SPA_TYPE_Float,
					info->volume.values, SPA_AUDIO_MAX_CHANNELS);
			SPA_FLAG_CLEAR(info->flags, VOLUME_HW_VOLUME);
			break;
		case SPA_PROP_volumeBase:
			if (spa_pod_get_float(&prop->value, &info->base) < 0)
				continue;
			break;
		case SPA_PROP_volumeStep:
		{
			float step;
			if (spa_pod_get_float(&prop->value, &step) >= 0)
				info->steps = 0x10000u * step;
			break;
		}
		case SPA_PROP_channelMap:
			info->map.channels = spa_pod_copy_array(&prop->value, SPA_TYPE_Id,
					info->map.map, SPA_AUDIO_MAX_CHANNELS);
			break;
		default:
			break;
		}
	}
	return 0;
}
