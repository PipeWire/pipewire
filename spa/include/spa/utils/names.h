/* Simple Plugin API
 *
 * Copyright © 2019 Wim Taymans
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

#ifndef SPA_UTILS_NAMES_H
#define SPA_UTILS_NAMES_H

#ifdef __cplusplus
extern "C" {
#endif

/** for factory names */
#define SPA_NAME_SUPPORT_CPU		"support.cpu"			/**< A CPU interface */
#define SPA_NAME_SUPPORT_DBUS		"support.dbus"			/**< A DBUS interface */
#define SPA_NAME_SUPPORT_LOG		"support.log"			/**< A Log interface */
#define SPA_NAME_SUPPORT_LOOP		"support.loop"			/**< A Loop/LoopControl/LoopUtils
									  *  interface */
#define SPA_NAME_SUPPORT_SYSTEM		"support.system"		/**< A System interface */


/* audio mixer */

#define SPA_NAME_AUDIO_MIXER		"audio.mix"			/**< mixes the raw audio on N input
									  *  ports together on the output
									  *  port */
/** audio processing */
#define SPA_NAME_AUDIO_PROCESS_FORMAT	"audio.process.format"		/**< processes raw audio from one format
									  *  to another */
#define SPA_NAME_AUDIO_PROCESS_CHANNELMIX	\
					"audio.process.channelmix"	/**< mixes raw audio channels and applies
									  *  volume change. */
#define SPA_NAME_AUDIO_PROCESS_RESAMPLE		\
					"audio.process.resample"	/**< resamples raw audio */
#define SPA_NAME_AUDIO_PROCESS_DEINTERLEAVE	\
					"audio.process.deinterleave"	/**< deinterleave raw audio channels */
#define SPA_NAME_AUDIO_PROCESS_INTERLEAVE	\
					"audio.process.interleave"	/**< interleave raw audio channels */


/** audio convert combines some of the audio processing */
#define SPA_NAME_AUDIO_CONVERT		"audio.convert"			/**< converts raw audio from one format
									  *  to another. Must include at least
									  *  format, channelmix and resample
									  *  processing */
#define SPA_NAME_AUDIO_ADAPT		"audio.adapt"			/**< combination of a node and an
									  *  audio.convert. Does clock slaving */

/** video processing */
#define SPA_NAME_VIDEO_PROCESS_FORMAT	"video.process.format"		/**< processes raw video from one format
									  *  to another */
#define SPA_NAME_VIDEO_PROCESS_SCALE	"video.process.scale"		/**< scales raw video */

/** video convert combines some of the video processing */
#define SPA_NAME_VIDEO_CONVERT		"video.convert"			/**< converts raw video from one format
									  *  to another. Must include at least
									  *  format and scaling */
#define SPA_NAME_VIDEO_ADAPT		"video.adapt"			/**< combination of a node and a
									  *  video.convert. */
/** keys for alsa factory names */
#define SPA_NAME_API_ALSA_MONITOR	"api.alsa.monitor"		/**< an alsa Monitor interface */
#define SPA_NAME_API_ALSA_DEVICE	"api.alsa.device"		/**< an alsa Device interface */
#define SPA_NAME_API_ALSA_PCM_SOURCE	"api.alsa.pcm.source"		/**< an alsa Node interface for
									  *  capturing PCM */
#define SPA_NAME_API_ALSA_PCM_SINK	"api.alsa.pcm.sink"		/**< an alsa Node interface for
									  *  playback PCM */

/** keys for bluez5 factory names */
#define SPA_NAME_API_BLUEZ5_MONITOR	"api.bluez5.monitor"		/**< a Monitor interface */
#define SPA_NAME_API_BLUEZ5_DEVICE	"api.bluez5.device"		/**< a Device interface */
#define SPA_NAME_API_BLUEZ5_A2DP_SINK	"api.bluez5.a2dp.sink"		/**< a playback Node interface for A2DP profiles */
#define SPA_NAME_API_BLUEZ5_A2DP_SOURCE	"api.bluez5.a2dp.source"	/**< a capture Node interface for A2DP profiles */
#define SPA_NAME_API_BLUEZ5_SCO_SINK	"api.bluez5.sco.sink"		/**< a playback Node interface for HSP/HFP profiles */
#define SPA_NAME_API_BLUEZ5_SCO_SOURCE	"api.bluez5.sco.source"		/**< a capture Node interface for HSP/HFP profiles */

/** keys for v4l2 factory names */
#define SPA_NAME_API_V4L2_MONITOR	"api.v4l2.monitor"		/**< a v4l2 Monitor interface */
#define SPA_NAME_API_V4L2_DEVICE	"api.v4l2.device"		/**< a v4l2 Device interface */
#define SPA_NAME_API_V4L2_SOURCE	"api.v4l2.source"		/**< a v4l2 Node interface for
									  *  capturing */

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* SPA_UTILS_NAMES_H */
