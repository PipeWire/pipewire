/* Simple Plugin API */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef SPA_VIDEO_COLOR_H
#define SPA_VIDEO_COLOR_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * \addtogroup spa_param
 * \{
 */

/**
 * Possible color range values. These constants are defined for 8 bit color
 * values and can be scaled for other bit depths.
 */
enum spa_video_color_range {
	SPA_VIDEO_COLOR_RANGE_UNKNOWN = 0,	/**< unknown range */
	SPA_VIDEO_COLOR_RANGE_0_255,		/**< [0..255] for 8 bit components */
	SPA_VIDEO_COLOR_RANGE_16_235		/**< [16..235] for 8 bit components. Chroma has
						     [16..240] range. */
};

/**
 * The color matrix is used to convert between Y'PbPr and
 * non-linear RGB (R'G'B')
 */
enum spa_video_color_matrix {
	SPA_VIDEO_COLOR_MATRIX_UNKNOWN = 0,	/**< unknown matrix */
	SPA_VIDEO_COLOR_MATRIX_RGB,		/**< identity matrix */
	SPA_VIDEO_COLOR_MATRIX_FCC,		/**< FCC color matrix */
	SPA_VIDEO_COLOR_MATRIX_BT709,		/**< ITU BT.709 color matrix */
	SPA_VIDEO_COLOR_MATRIX_BT601,		/**< ITU BT.601 color matrix */
	SPA_VIDEO_COLOR_MATRIX_SMPTE240M,	/**< SMTPE  240M color matrix */
	SPA_VIDEO_COLOR_MATRIX_BT2020,		/**<  ITU-R BT.2020 color matrix */
};

/**
 * The video transfer function defines the formula for converting between
 * non-linear RGB (R'G'B') and linear RGB
 */
enum spa_video_transfer_function {
	SPA_VIDEO_TRANSFER_UNKNOWN = 0,	/**< unknown transfer function */
	SPA_VIDEO_TRANSFER_GAMMA10,	/**< linear RGB, gamma 1.0 curve */
	SPA_VIDEO_TRANSFER_GAMMA18,	/**< Gamma 1.8 curve */
	SPA_VIDEO_TRANSFER_GAMMA20,	/**< Gamma 2.0 curve */
	SPA_VIDEO_TRANSFER_GAMMA22,	/**< Gamma 2.2 curve */
	SPA_VIDEO_TRANSFER_BT709,	/**< Gamma 2.2 curve with a linear segment in the lower range */
	SPA_VIDEO_TRANSFER_SMPTE240M,	/**< Gamma 2.2 curve with a linear segment in the lower range */
	SPA_VIDEO_TRANSFER_SRGB,	/**< Gamma 2.4 curve with a linear segment in the lower range */
	SPA_VIDEO_TRANSFER_GAMMA28,	/**< Gamma 2.8 curve */
	SPA_VIDEO_TRANSFER_LOG100,	/**< Logarithmic transfer characteristic 100:1 range */
	SPA_VIDEO_TRANSFER_LOG316,	/**< Logarithmic transfer characteristic 316.22777:1 range */
	SPA_VIDEO_TRANSFER_BT2020_12,	/**< Gamma 2.2 curve with a linear segment in the lower
					 *   range. Used for BT.2020 with 12 bits per
					 *   component */
	SPA_VIDEO_TRANSFER_ADOBERGB,	/**< Gamma 2.19921875 */
	SPA_VIDEO_TRANSFER_BT2020_10,	/**< Rec. ITU-R BT.2020-2 with 10 bits per component.
					 *   (functionally the same as the values
					 * SPA_VIDEO_TRANSFER_BT709 and SPA_VIDEO_TRANSFER_BT601) */
	SPA_VIDEO_TRANSFER_SMPTE2084,	/**< SMPTE ST 2084 for 10, 12, 14, and 16-bit systems.
					 * Known as perceptual quantization (PQ) */
	SPA_VIDEO_TRANSFER_ARIB_STD_B67,/**< Association of Radio Industries and Businesses (ARIB)
					 * STD-B67 and Rec. ITU-R BT.2100-1 hybrid loggamma (HLG) system */
	SPA_VIDEO_TRANSFER_BT601,	/**< also known as SMPTE170M / ITU-R BT1358 525 or 625 / ITU-R BT1700 NTSC
					 * Functionally the same as the values
					 * SPA_VIDEO_TRANSFER_BT709, and SPA_VIDEO_TRANSFER_BT2020_10 */
};

/**
 * The color primaries define the how to transform linear RGB values to and from
 * the CIE XYZ colorspace.
 */
enum spa_video_color_primaries {
	SPA_VIDEO_COLOR_PRIMARIES_UNKNOWN = 0,	/**< unknown color primaries */
	SPA_VIDEO_COLOR_PRIMARIES_BT709,	/**< BT709 primaries */
	SPA_VIDEO_COLOR_PRIMARIES_BT470M,	/**< BT470M primaries */
	SPA_VIDEO_COLOR_PRIMARIES_BT470BG,	/**< BT470BG primaries */
	SPA_VIDEO_COLOR_PRIMARIES_SMPTE170M,	/**< SMPTE170M primaries */
	SPA_VIDEO_COLOR_PRIMARIES_SMPTE240M,	/**< SMPTE240M primaries */
	SPA_VIDEO_COLOR_PRIMARIES_FILM,		/**< Generic film */
	SPA_VIDEO_COLOR_PRIMARIES_BT2020,	/**< BT2020 primaries */
	SPA_VIDEO_COLOR_PRIMARIES_ADOBERGB,	/**< Adobe RGB primaries */
	SPA_VIDEO_COLOR_PRIMARIES_SMPTEST428,	/**< SMPTE ST 428 primaries (CIE 1931 XYZ) */
	SPA_VIDEO_COLOR_PRIMARIES_SMPTERP431,	/**< SMPTE RP 431 primaries (ST 431-2 (2011) / DCI P3) */
	SPA_VIDEO_COLOR_PRIMARIES_SMPTEEG432,	/**< SMPTE EG 432 primaries (ST 432-1 (2010) / P3 D65) */
	SPA_VIDEO_COLOR_PRIMARIES_EBU3213,	/**< EBU 3213 primaries (JEDEC P22 phosphors) */
};

/**
 * spa_video_colorimetry:
 *
 * Structure describing the color info.
 */
struct spa_video_colorimetry {
	enum spa_video_color_range range;	/**< The color range. This is the valid range for the
						 *    samples. It is used to convert the samples to Y'PbPr
						 *    values. */
	enum spa_video_color_matrix matrix;	/**< the color matrix. Used to convert between Y'PbPr and
						 *    non-linear RGB (R'G'B') */
	enum spa_video_transfer_function transfer; /**< The transfer function. Used to convert between
						    *   R'G'B' and RGB */
	enum spa_video_color_primaries primaries; /**< Color primaries. Used to convert between R'G'B'
						   *   and CIE XYZ */
};

/**
 * \}
 */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SPA_VIDEO_COLOR_H */
