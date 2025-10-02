/* Simple Plugin API */
/* SPDX-FileCopyrightText: Copyright © 2025 Carlos Rafael Giani */
/* SPDX-License-Identifier: MIT */

#ifndef SPA_AUDIO_MPEGH_H
#define SPA_AUDIO_MPEGH_H

#include <spa/param/audio/raw.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * \addtogroup spa_param
 * \{
 */

/**
 * MPEG-H 3D audio info.
 *
 * MPEG-H content is assumed to be provided in the form of an MPEG-H
 * 3D Audio Stream (MHAS). MHAS is a lightweight bitstream format that
 * encapsulates MPEG-H 3D Audio frames along with associated metadata.
 * It serves a similar role to the Annex B byte stream format used for
 * H.264, providing framing and synchronization for MPEG-H frames.
 *
 * MPEG-H is documented in the ISO/IEC 23008-3 specification.
 * MHAS is specified in ISO/IEC 23008-3, Clause 14.
 *
 * Note that unlike other formats, this one does not specify a channel
 * count. This is because MPEG-H is entity-based; it contains multiple
 * entities of different types (channel beds, audio objects etc.) which
 * do not map 1:1 to channels. The channel amount is determined by
 * decoders instead, based on the audio scene content and the target
 * playback system.
 */
struct spa_audio_info_mpegh {
	uint32_t rate;				/*< sample rate */
};

#define SPA_AUDIO_INFO_MPEGH_INIT(...)		((struct spa_audio_info_mpegh) { __VA_ARGS__ })

/**
 * \}
 */

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* SPA_AUDIO_MPEGH_H */
