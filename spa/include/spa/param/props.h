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

#ifndef __SPA_PARAM_PROPS_H__
#define __SPA_PARAM_PROPS_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/param/param.h>

/** properties of SPA_TYPE_OBJECT_PropInfo */
enum spa_prop_info {
	SPA_PROP_INFO_START,		/**< id of object, one of enum spa_param_type */
	SPA_PROP_INFO_id,		/**< associated id of the property */
	SPA_PROP_INFO_name,		/**< name of the property */
	SPA_PROP_INFO_type,		/**< type and range/enums of property */
	SPA_PROP_INFO_labels,		/**< labels of property if any, this is a
					  *  struct with pairs of values, the first one
					  *  is of the type of the property, the second
					  *  one is a string with a user readable label
					  *  for the value. */
};

/** predefined properties for SPA_TYPE_OBJECT_Props */
enum spa_prop {
	SPA_PROP_START,			/**< id of object, one of enum spa_param_type */

	SPA_PROP_unknown,		/**< an unknown property */

	SPA_PROP_device,
	SPA_PROP_deviceName,
	SPA_PROP_deviceFd,
	SPA_PROP_card,
	SPA_PROP_cardName,

	SPA_PROP_minLatency,
	SPA_PROP_maxLatency,
	SPA_PROP_periods,
	SPA_PROP_periodSize,
	SPA_PROP_periodEvent,
	SPA_PROP_live,

	SPA_PROP_waveType,
	SPA_PROP_frequency,
	SPA_PROP_volume,
	SPA_PROP_mute,
	SPA_PROP_patternType,
	SPA_PROP_ditherType,
	SPA_PROP_truncate,

	SPA_PROP_brightness,
	SPA_PROP_contrast,
	SPA_PROP_saturation,
	SPA_PROP_hue,
	SPA_PROP_gamma,
	SPA_PROP_exposure,
	SPA_PROP_gain,
	SPA_PROP_sharpness,

	SPA_PROP_START_CUSTOM	= 0x10000,
};

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_PARAM_PROPS_H__ */
