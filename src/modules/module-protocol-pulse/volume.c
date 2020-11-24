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

struct volume {
	uint8_t channels;
	float values[CHANNELS_MAX];
};

#define VOLUME_INIT	(struct volume) {		\
				.channels = 0,		\
			}

#define VOLUME_DEFAULT	(struct volume) {		\
				.channels = 2,		\
				.values[0] = 1.0f,	\
				.values[1] = 1.0f,	\
			}

static inline bool volume_valid(const struct volume *vol)
{
	if (vol->channels == 0 || vol->channels > CHANNELS_MAX)
		return false;
	return true;
}

struct volume_info {
	struct volume volume;
	struct channel_map map;
	bool mute;
	float level;
	float base;
	uint32_t steps;
#define VOLUME_HW_VOLUME	(1<<0)
#define VOLUME_HW_MUTE		(1<<1)
	uint32_t flags;
};

#define VOLUME_INFO_INIT	(struct volume_info) {		\
					.volume = VOLUME_INIT,	\
					.mute = false,		\
					.level = 1.0,		\
					.base = 1.0,		\
					.steps = 256,		\
				}


static int volume_parse_param(const struct spa_pod *param, struct volume_info *info)
{
	struct spa_pod_object *obj = (struct spa_pod_object *) param;
	struct spa_pod_prop *prop;

	SPA_POD_OBJECT_FOREACH(obj, prop) {
		switch (prop->key) {
		case SPA_PROP_volume:
			spa_pod_get_float(&prop->value, &info->level);
			SPA_FLAG_UPDATE(info->flags, VOLUME_HW_VOLUME,
                                        prop->flags & SPA_POD_PROP_FLAG_HARDWARE);

			break;
		case SPA_PROP_mute:
			spa_pod_get_bool(&prop->value, &info->mute);
			SPA_FLAG_UPDATE(info->flags, VOLUME_HW_MUTE,
                                        prop->flags & SPA_POD_PROP_FLAG_HARDWARE);
			break;
		case SPA_PROP_channelVolumes:
			info->volume.channels = spa_pod_copy_array(&prop->value, SPA_TYPE_Float,
					info->volume.values, SPA_AUDIO_MAX_CHANNELS);
			SPA_FLAG_UPDATE(info->flags, VOLUME_HW_VOLUME,
                                        prop->flags & SPA_POD_PROP_FLAG_HARDWARE);
			break;
		case SPA_PROP_volumeBase:
			spa_pod_get_float(&prop->value, &info->base);
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
