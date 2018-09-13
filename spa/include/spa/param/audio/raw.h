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

#ifndef __SPA_AUDIO_RAW_H__
#define __SPA_AUDIO_RAW_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <endian.h>

#define SPA_AUDIO_MAX_CHANNELS	64

enum spa_audio_format {
	SPA_AUDIO_FORMAT_UNKNOWN,
	SPA_AUDIO_FORMAT_ENCODED,

	/* interleaved formats */
	SPA_AUDIO_FORMAT_START_Interleaved	= 0x100,
	SPA_AUDIO_FORMAT_S8,
	SPA_AUDIO_FORMAT_U8,
	SPA_AUDIO_FORMAT_S16_LE,
	SPA_AUDIO_FORMAT_S16_BE,
	SPA_AUDIO_FORMAT_U16_LE,
	SPA_AUDIO_FORMAT_U16_BE,
	SPA_AUDIO_FORMAT_S24_32_LE,
	SPA_AUDIO_FORMAT_S24_32_BE,
	SPA_AUDIO_FORMAT_U24_32_LE,
	SPA_AUDIO_FORMAT_U24_32_BE,
	SPA_AUDIO_FORMAT_S32_LE,
	SPA_AUDIO_FORMAT_S32_BE,
	SPA_AUDIO_FORMAT_U32_LE,
	SPA_AUDIO_FORMAT_U32_BE,
	SPA_AUDIO_FORMAT_S24_LE,
	SPA_AUDIO_FORMAT_S24_BE,
	SPA_AUDIO_FORMAT_U24_LE,
	SPA_AUDIO_FORMAT_U24_BE,
	SPA_AUDIO_FORMAT_S20_LE,
	SPA_AUDIO_FORMAT_S20_BE,
	SPA_AUDIO_FORMAT_U20_LE,
	SPA_AUDIO_FORMAT_U20_BE,
	SPA_AUDIO_FORMAT_S18_LE,
	SPA_AUDIO_FORMAT_S18_BE,
	SPA_AUDIO_FORMAT_U18_LE,
	SPA_AUDIO_FORMAT_U18_BE,
	SPA_AUDIO_FORMAT_F32_LE,
	SPA_AUDIO_FORMAT_F32_BE,
	SPA_AUDIO_FORMAT_F64_LE,
	SPA_AUDIO_FORMAT_F64_BE,

	/* planar formats */
	SPA_AUDIO_FORMAT_START_Planar		= 0x200,
	SPA_AUDIO_FORMAT_U8P,
	SPA_AUDIO_FORMAT_S16P,
	SPA_AUDIO_FORMAT_S24_32P,
	SPA_AUDIO_FORMAT_S32P,
	SPA_AUDIO_FORMAT_S24P,
	SPA_AUDIO_FORMAT_F32P,
	SPA_AUDIO_FORMAT_F64P,

	SPA_AUDIO_FORMAT_START_Other		= 0x300,

#if __BYTE_ORDER == __BIG_ENDIAN
	SPA_AUDIO_FORMAT_S16 = SPA_AUDIO_FORMAT_S16_BE,
	SPA_AUDIO_FORMAT_U16 = SPA_AUDIO_FORMAT_U16_BE,
	SPA_AUDIO_FORMAT_S24_32 = SPA_AUDIO_FORMAT_S24_32_BE,
	SPA_AUDIO_FORMAT_U24_32 = SPA_AUDIO_FORMAT_U24_32_BE,
	SPA_AUDIO_FORMAT_S32 = SPA_AUDIO_FORMAT_S32_BE,
	SPA_AUDIO_FORMAT_U32 = SPA_AUDIO_FORMAT_U32_BE,
	SPA_AUDIO_FORMAT_S24 = SPA_AUDIO_FORMAT_S24_BE,
	SPA_AUDIO_FORMAT_U24 = SPA_AUDIO_FORMAT_U24_BE,
	SPA_AUDIO_FORMAT_S20 = SPA_AUDIO_FORMAT_S20_BE,
	SPA_AUDIO_FORMAT_U20 = SPA_AUDIO_FORMAT_U20_BE,
	SPA_AUDIO_FORMAT_S18 = SPA_AUDIO_FORMAT_S18_BE,
	SPA_AUDIO_FORMAT_U18 = SPA_AUDIO_FORMAT_U18_BE,
	SPA_AUDIO_FORMAT_F32 = SPA_AUDIO_FORMAT_F32_BE,
	SPA_AUDIO_FORMAT_F64 = SPA_AUDIO_FORMAT_F64_BE,
	SPA_AUDIO_FORMAT_S16_OE = SPA_AUDIO_FORMAT_S16_LE,
	SPA_AUDIO_FORMAT_U16_OE = SPA_AUDIO_FORMAT_U16_LE,
	SPA_AUDIO_FORMAT_S24_32_OE = SPA_AUDIO_FORMAT_S24_32_LE,
	SPA_AUDIO_FORMAT_U24_32_OE = SPA_AUDIO_FORMAT_U24_32_LE,
	SPA_AUDIO_FORMAT_S32_OE = SPA_AUDIO_FORMAT_S32_LE,
	SPA_AUDIO_FORMAT_U32_OE = SPA_AUDIO_FORMAT_U32_LE,
	SPA_AUDIO_FORMAT_S24_OE = SPA_AUDIO_FORMAT_S24_LE,
	SPA_AUDIO_FORMAT_U24_OE = SPA_AUDIO_FORMAT_U24_LE,
	SPA_AUDIO_FORMAT_S20_OE = SPA_AUDIO_FORMAT_S20_LE,
	SPA_AUDIO_FORMAT_U20_OE = SPA_AUDIO_FORMAT_U20_LE,
	SPA_AUDIO_FORMAT_S18_OE = SPA_AUDIO_FORMAT_S18_LE,
	SPA_AUDIO_FORMAT_U18_OE = SPA_AUDIO_FORMAT_U18_LE,
	SPA_AUDIO_FORMAT_F32_OE = SPA_AUDIO_FORMAT_F32_LE,
	SPA_AUDIO_FORMAT_F64_OE = SPA_AUDIO_FORMAT_F64_LE,
#elif __BYTE_ORDER == __LITTLE_ENDIAN
	SPA_AUDIO_FORMAT_S16 = SPA_AUDIO_FORMAT_S16_LE,
	SPA_AUDIO_FORMAT_U16 = SPA_AUDIO_FORMAT_U16_LE,
	SPA_AUDIO_FORMAT_S24_32 = SPA_AUDIO_FORMAT_S24_32_LE,
	SPA_AUDIO_FORMAT_U24_32 = SPA_AUDIO_FORMAT_U24_32_LE,
	SPA_AUDIO_FORMAT_S32 = SPA_AUDIO_FORMAT_S32_LE,
	SPA_AUDIO_FORMAT_U32 = SPA_AUDIO_FORMAT_U32_LE,
	SPA_AUDIO_FORMAT_S24 = SPA_AUDIO_FORMAT_S24_LE,
	SPA_AUDIO_FORMAT_U24 = SPA_AUDIO_FORMAT_U24_LE,
	SPA_AUDIO_FORMAT_S20 = SPA_AUDIO_FORMAT_S20_LE,
	SPA_AUDIO_FORMAT_U20 = SPA_AUDIO_FORMAT_U20_LE,
	SPA_AUDIO_FORMAT_S18 = SPA_AUDIO_FORMAT_S18_LE,
	SPA_AUDIO_FORMAT_U18 = SPA_AUDIO_FORMAT_U18_LE,
	SPA_AUDIO_FORMAT_F32 = SPA_AUDIO_FORMAT_F32_LE,
	SPA_AUDIO_FORMAT_F64 = SPA_AUDIO_FORMAT_F64_LE,
	SPA_AUDIO_FORMAT_S16_OE = SPA_AUDIO_FORMAT_S16_BE,
	SPA_AUDIO_FORMAT_U16_OE = SPA_AUDIO_FORMAT_U16_BE,
	SPA_AUDIO_FORMAT_S24_32_OE = SPA_AUDIO_FORMAT_S24_32_BE,
	SPA_AUDIO_FORMAT_U24_32_OE = SPA_AUDIO_FORMAT_U24_32_BE,
	SPA_AUDIO_FORMAT_S32_OE = SPA_AUDIO_FORMAT_S32_BE,
	SPA_AUDIO_FORMAT_U32_OE = SPA_AUDIO_FORMAT_U32_BE,
	SPA_AUDIO_FORMAT_S24_OE = SPA_AUDIO_FORMAT_S24_BE,
	SPA_AUDIO_FORMAT_U24_OE = SPA_AUDIO_FORMAT_U24_BE,
	SPA_AUDIO_FORMAT_S20_OE = SPA_AUDIO_FORMAT_S20_BE,
	SPA_AUDIO_FORMAT_U20_OE = SPA_AUDIO_FORMAT_U20_BE,
	SPA_AUDIO_FORMAT_S18_OE = SPA_AUDIO_FORMAT_S18_BE,
	SPA_AUDIO_FORMAT_U18_OE = SPA_AUDIO_FORMAT_U18_BE,
	SPA_AUDIO_FORMAT_F32_OE = SPA_AUDIO_FORMAT_F32_BE,
	SPA_AUDIO_FORMAT_F64_OE = SPA_AUDIO_FORMAT_F64_BE,
#endif
};

#define SPA_AUDIO_FORMAT_IS_INTERLEAVED(fmt)	((fmt) > SPA_AUDIO_FORMAT_START_Interleaved && (fmt) < SPA_AUDIO_FORMAT_START_Planar)
#define SPA_AUDIO_FORMAT_IS_PLANAR(fmt)		((fmt) > SPA_AUDIO_FORMAT_START_Planar && (fmt) < SPA_AUDIO_FORMAT_START_Other)

enum spa_audio_channel {
	SPA_AUDIO_CHANNEL_UNKNOWN,	/**< unspecified */
	SPA_AUDIO_CHANNEL_NA,		/**< N/A, silent */
	SPA_AUDIO_CHANNEL_MONO,         /**< mono stream */
	SPA_AUDIO_CHANNEL_FL,           /**< front left */
	SPA_AUDIO_CHANNEL_FR,           /**< front right */
	SPA_AUDIO_CHANNEL_RL,           /**< rear left */
	SPA_AUDIO_CHANNEL_RR,           /**< rear right */
	SPA_AUDIO_CHANNEL_FC,           /**< front center */
	SPA_AUDIO_CHANNEL_LFE,          /**< LFE */
	SPA_AUDIO_CHANNEL_SL,           /**< side left */
	SPA_AUDIO_CHANNEL_SR,           /**< side right */
	SPA_AUDIO_CHANNEL_RC,           /**< rear center */
	SPA_AUDIO_CHANNEL_FLC,          /**< front left center */
	SPA_AUDIO_CHANNEL_FRC,          /**< front right center */
	SPA_AUDIO_CHANNEL_RLC,          /**< rear left center */
	SPA_AUDIO_CHANNEL_RRC,          /**< rear right center */
	SPA_AUDIO_CHANNEL_FLW,          /**< front left wide */
	SPA_AUDIO_CHANNEL_FRW,          /**< front right wide */
	SPA_AUDIO_CHANNEL_FLH,          /**< front left high */
	SPA_AUDIO_CHANNEL_FCH,          /**< front center high */
	SPA_AUDIO_CHANNEL_FRH,          /**< front right high */
	SPA_AUDIO_CHANNEL_TC,           /**< top center */
	SPA_AUDIO_CHANNEL_TFL,          /**< top front left */
	SPA_AUDIO_CHANNEL_TFR,          /**< top front right */
	SPA_AUDIO_CHANNEL_TFC,          /**< top front center */
	SPA_AUDIO_CHANNEL_TRL,          /**< top rear left */
	SPA_AUDIO_CHANNEL_TRR,          /**< top rear right */
	SPA_AUDIO_CHANNEL_TRC,          /**< top rear center */
	SPA_AUDIO_CHANNEL_TFLC,         /**< top front left center */
	SPA_AUDIO_CHANNEL_TFRC,         /**< top front right center */
	SPA_AUDIO_CHANNEL_TSL,          /**< top side left */
	SPA_AUDIO_CHANNEL_TSR,          /**< top side right */
	SPA_AUDIO_CHANNEL_LLFE,         /**< left LFE */
	SPA_AUDIO_CHANNEL_RLFE,         /**< right LFE */
	SPA_AUDIO_CHANNEL_BC,           /**< bottom center */
	SPA_AUDIO_CHANNEL_BLC,          /**< bottom left center */
	SPA_AUDIO_CHANNEL_BRC,          /**< bottom right center */

	SPA_AUDIO_CHANNEL_CUSTOM_START	= 0x10000,
};

#define SPA_AUDIO_CHANNEL_MASK(ch)	(1LL << SPA_AUDIO_CHANNEL_ ## ch)

enum spa_audio_layout {
	SPA_AUDIO_LAYOUT_DEFAULT,	/**< default layout for the number of channels */
	SPA_AUDIO_LAYOUT_CUSTOM,	/**< custom layout, either the order of the channels is
					  *  non-standard or the channel position array contains
					  *  custom channels. */
	SPA_AUDIO_LAYOUT_MONO,		/**< MONO */
	SPA_AUDIO_LAYOUT_STEREO,	/**< FL FR */
	SPA_AUDIO_LAYOUT_2_1,		/**< FL FR LFE */
	SPA_AUDIO_LAYOUT_2RC,		/**< FL FR RC */
	SPA_AUDIO_LAYOUT_2FC,		/**< FL FR FC */
	SPA_AUDIO_LAYOUT_3_1,		/**< FL FR FC LFE */
	SPA_AUDIO_LAYOUT_4_0,		/**< FL FR FC RC */
	SPA_AUDIO_LAYOUT_4_1,		/**< FL FR FC RC LFE */
	SPA_AUDIO_LAYOUT_2_2,		/**< FL FR SL SR */
	SPA_AUDIO_LAYOUT_QUAD,		/**< FL FR RL RR */
	SPA_AUDIO_LAYOUT_5_0,		/**< FL FR FC SL SR */
	SPA_AUDIO_LAYOUT_5_1,		/**< FL FR FC SL SR LFE */
	SPA_AUDIO_LAYOUT_5_0R,		/**< FL FR FC RL RR */
	SPA_AUDIO_LAYOUT_5_1R,		/**< FL FR FC RL RR LFE */
	SPA_AUDIO_LAYOUT_6_0,		/**< FL FR FC SL SR RC */
	SPA_AUDIO_LAYOUT_6_0F,		/**< FL FR SL SR FLC FRC */
	SPA_AUDIO_LAYOUT_HEXAGONAL,	/**< FL FR FC RL RR RC */
	SPA_AUDIO_LAYOUT_6_1,		/**< FL FR FC SL SR LFE RC */
	SPA_AUDIO_LAYOUT_6_1R,		/**< FL FR FC RL RR LFE RC */
	SPA_AUDIO_LAYOUT_6_1F,		/**< FL FR SL SR FLC FRC LFE */
	SPA_AUDIO_LAYOUT_7_0,		/**< FL FR FC SL SR RL RR */
	SPA_AUDIO_LAYOUT_7_0F,		/**< FL FR FC SL SR FLC FRC */
	SPA_AUDIO_LAYOUT_7_1,		/**< FL FR FC SL SR LFE RL RR */
	SPA_AUDIO_LAYOUT_7_1W,		/**< FL FR FC SL SR LFE FLC FRC */
	SPA_AUDIO_LAYOUT_7_1WR,		/**< FL FR FC RL RR LFE FLC FRC */
	SPA_AUDIO_LAYOUT_OCTAGONAL,	/**< FL FR FC SL SR RL RC RR */
};

/** Extra audio flags */
#define SPA_AUDIO_FLAG_NONE		(0)		/*< no valid flag */
#define SPA_AUDIO_FLAG_UNPOSITIONED	(1 << 0)	/*< the position array explicitly
							 *  contains unpositioned channels. */
/** Audio information description */
struct spa_audio_info_raw {
	enum spa_audio_format format;		/*< format, one of enum spa_audio_format */
	uint32_t flags;				/*< extra flags */
	uint32_t rate;				/*< sample rate */
	uint32_t channels;			/*< number of channels */
	uint32_t position[SPA_AUDIO_MAX_CHANNELS];	/*< channel position from enum spa_audio_channel */
};

#define SPA_AUDIO_INFO_RAW_INIT(...)		(struct spa_audio_info_raw) { __VA_ARGS__ }

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_AUDIO_RAW_H__ */
