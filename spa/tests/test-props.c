/* Spa
 * Copyright (C) 2017 Wim Taymans <wim.taymans@gmail.com>
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

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>

#include <spa/support/log-impl.h>
#include <spa/pod/pod.h>
#include <spa/pod/builder.h>
#include <spa/pod/parser.h>
#include <spa/param/video/format-utils.h>
#include <spa/debug/pod.h>
#include <spa/debug/format.h>
#include <spa/debug/types.h>

#if 0
/* { video/raw,
 *   format: (int 1) { 1  2 },
 *   size:   (rect (320 240)) { (1 1) (MAX MAX) },
 *   framerate: (frac (25 1)) { (0 1) (MAX 1) },
 *
 * "(object $format
 * 	(id $video) (id $raw)
 *      (prop $format    (int $I420) { $I420 $YUY2 } )
 *      (prop $size      (rect (320 240)) [ (1 1) (MAX MAX) ] )
 *      (prop $framerate (frac (25 1))))
 * "
 *
 */

bool:      true | false
int:       <num>
float:     <num.frac>
string:    "<string>"
bytes:     "$<base64>"
pointer:   "@<pointer>"
rectangle: "<width>x<height>"
fraction:  "<num>/<denom>"
bitmask:   "|<bits>"
array:     "[<values>,...]"
object:    "{<key>: <value>,...}"
key:       "<name>"


  "format": "S16LE",
  "format": [ "S16LE", "S24LE" ],
  "size": "320x240",
  "+size": [ "320x240", { "min": "320x240", "max": "640x480", "step": "8x8" ],
  "-size": [ "320x240", ["320x240", "640x480", "512x400"] ],

  "schema.size": { "default": "320x240",
		   "flags": ["range", "optional"],
		   "values": ["320x240", "640x480", "8x8"] }


  "size-schema":
	{ "default": "320x240",
	  "values": ["320x240", "640x480"]
	},
  "size-schema":
	{ "default": "320x240",
	  "values": { "min": "320x240", "max": "640x480", "step": "8x8" }
	},


  "size-alt": { "def": "320x240", "type": "range", "vals": ["320x240", "640x480", "8x8"] },
  "size-alt": { "def": "320x240", "type": "enum", "vals": ["320x240", "640x480"],

{
  "type": "audio/raw",
  "format": ["S16LE", "enum", "S16LE", "S24LE" ],
  "size": [ "320x240", "range", "320x240", "640x480" ],
  "framerate": [ "25/1", "range", "25/1", "30/1" ]
}

{ "type": "audio", "subtype": "raw",
  "format": "S16LE",
  "size": { "min": "320x240", "max": "640x480", "step": "8x8" },

  "framerate": [ "25/1", "30/1" ] }

* JSON based format description

 [ <type>,
   [ <media-type>, <media-subtype> ],
   {
     <key> : [ <type>, <value>, [ <value>, ... ] ],
     ...
   }
 ]

   <type> = "123.."

   1: s = string    :  "value"
      i = int       :  <number>
      f = float     :  <float>
      b = bool      :  true | false
      R = rectangle : [ <width>, <height> ]
      F = fraction  : [ <num>, <denom> ]

   2: - = default (only default value present)
      e = enum	        : [ <value>, ... ]
      f = flags	        : [ <number> ]
      m = min/max	: [ <min>, <max> ]
      s = min/max/step  : [ <min>, <max>, <step> ]

   3: u = unset		: value is unset, choose from options or default
      o = optional	: value does not need to be set
      r = readonly      : value is read only
      d = deprecated    : value is deprecated

[ "Format",
  [ "video", "raw"],
  {
    "format" :    [ "se", "I420", [ "I420", "YUY2" ] ],
    "size" :      [ "Rmu", [320, 240], [[ 640, 480], [1024,786]]],
    "framerate" : [ "Fsu", [25, 1], [[ 0, 1], [65536,1]]]
  }
]

[ "Format",
  [ "audio", "raw"],
  {
    "format" :      [ "se", "S16LE", [ "F32LE", "S16LE" ] ],
    "rate" :        [ "imu", 44100, [8000, 96000]],
    "channels" :    [ "imu", 1, [1, 4096]]
    "interleaved" : [ "beo", true ]
  }
]

spa_build(SPA_MEDIA_TYPE_VIDEO, SPA_MEDIA_SUBTYPE_RAW,
	  SPA_FORMAT_VIDEO_format, SPA_PROP_TYPE_ID,
	  video_format.I420
	  SPA_POD_PROP_FLAG_UNSET |
	  SPA_PROP_RANGE_ENUM, 2,
	  video_format.I420,
	  video_format.YUY2,
	  SPA_FORMAT_VIDEO_size, SPA_PROP_TYPE_RECTANGLE,
	  320, 240,
	  SPA_POD_PROP_FLAG_UNSET |
	  SPA_PROP_RANGE_MIN_MAX,
	  1, 1,
	  INT32_MAX, INT32_MAX,
	  SPA_FORMAT_VIDEO_framerate, SPA_PROP_TYPE_FRACTION, 25, 1,
	  SPA_POD_PROP_FLAG_UNSET | SPA_PROP_RANGE_MIN_MAX, 0, 1, INT32_MAX, 1, 0);
#endif

static void do_static_struct(void)
{
	struct _test_format {
		struct spa_pod_object fmt;

		struct {
			struct spa_pod_enum media_type		SPA_ALIGNED(8);
			struct spa_pod_enum media_subtype		SPA_ALIGNED(8);

			struct spa_pod_prop prop_format		SPA_ALIGNED(8);
			struct {
				uint32_t def_format;
				uint32_t enum_format[2];
			} format_vals;

			struct spa_pod_prop prop_size		SPA_ALIGNED(8);
			struct {
				struct spa_rectangle def_size;
				struct spa_rectangle min_size;
				struct spa_rectangle max_size;
			} size_vals;

			struct spa_pod_prop prop_framerate	SPA_ALIGNED(8);
			struct {
				struct spa_fraction def_framerate;
				struct spa_fraction min_framerate;
				struct spa_fraction max_framerate;
			} framerate_vals;
		} props;
	} test_format = {
		SPA_POD_OBJECT_INIT(sizeof(test_format.props) + sizeof(struct spa_pod_object_body),
				SPA_TYPE_OBJECT_Format, 0),
		{
			SPA_POD_ENUM_INIT(SPA_MEDIA_TYPE_video),
			SPA_POD_ENUM_INIT(SPA_MEDIA_SUBTYPE_raw),

			SPA_POD_PROP_INIT(sizeof(test_format.props.format_vals) +
						sizeof(struct spa_pod_prop_body),
					  SPA_FORMAT_VIDEO_format,
					  SPA_POD_PROP_RANGE_ENUM | SPA_POD_PROP_FLAG_UNSET,
					  sizeof(uint32_t), SPA_TYPE_Enum),
			{
				SPA_VIDEO_FORMAT_I420,
				{ SPA_VIDEO_FORMAT_I420, SPA_VIDEO_FORMAT_YUY2 }
			},
			SPA_POD_PROP_INIT(sizeof(test_format.props.size_vals) +
						sizeof(struct spa_pod_prop_body),
					  SPA_FORMAT_VIDEO_size,
					  SPA_POD_PROP_RANGE_MIN_MAX | SPA_POD_PROP_FLAG_UNSET,
					  sizeof(struct spa_rectangle), SPA_TYPE_Rectangle),

			{
				SPA_RECTANGLE(320,243),
				SPA_RECTANGLE(1,1), SPA_RECTANGLE(INT32_MAX, INT32_MAX)
			},
			SPA_POD_PROP_INIT(sizeof(test_format.props.framerate_vals) +
						sizeof(struct spa_pod_prop_body),
					  SPA_FORMAT_VIDEO_framerate,
					  SPA_POD_PROP_RANGE_MIN_MAX | SPA_POD_PROP_FLAG_UNSET,
					  sizeof(struct spa_fraction), SPA_TYPE_Fraction),
			{
				SPA_FRACTION(25,1),
				SPA_FRACTION(0,1), SPA_FRACTION(INT32_MAX,1)
			}
		}
	};

	spa_debug_pod(0, NULL, &test_format.fmt.pod);
	spa_debug_format(0, NULL, &test_format.fmt.pod);

	{
		uint32_t format = -1;
		int res;
		struct spa_fraction frac = { -1, -1 };

		res = spa_pod_object_parse(&test_format.fmt.pod,
			":", SPA_FORMAT_VIDEO_format, "I", &format,
			":", SPA_FORMAT_VIDEO_framerate, "F", &frac, NULL);

		printf("%d format %d num %d denom %d\n", res, format, frac.num, frac.denom);

		spa_pod_fixate(&test_format.fmt.pod);

		res = spa_pod_object_parse(&test_format.fmt.pod,
			":", SPA_FORMAT_VIDEO_format, "I", &format,
			":", SPA_FORMAT_VIDEO_framerate, "F", &frac, NULL);

		printf("%d format %d num %d denom %d\n", res, format, frac.num, frac.denom);
	}

}

int main(int argc, char *argv[])
{
	struct spa_pod_builder b = { NULL, };
	uint8_t buffer[1024];
	struct spa_pod_object *fmt;

	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	spa_pod_builder_push_object(&b, SPA_TYPE_OBJECT_Format, 0);

	spa_pod_builder_enum(&b, SPA_MEDIA_TYPE_video);
	spa_pod_builder_enum(&b, SPA_MEDIA_SUBTYPE_raw);

	spa_pod_builder_push_prop(&b, SPA_FORMAT_VIDEO_format,
				  SPA_POD_PROP_RANGE_ENUM | SPA_POD_PROP_FLAG_UNSET);
	spa_pod_builder_enum(&b, SPA_VIDEO_FORMAT_I420);
	spa_pod_builder_enum(&b, SPA_VIDEO_FORMAT_I420);
	spa_pod_builder_enum(&b, SPA_VIDEO_FORMAT_YUY2);
	spa_pod_builder_pop(&b);

	struct spa_rectangle size_min_max[] = { {1, 1}, {INT32_MAX, INT32_MAX} };
	spa_pod_builder_push_prop(&b,
				  SPA_FORMAT_VIDEO_size,
				  SPA_POD_PROP_RANGE_MIN_MAX | SPA_POD_PROP_FLAG_UNSET);
	spa_pod_builder_rectangle(&b, 320, 240);
	spa_pod_builder_raw(&b, size_min_max, sizeof(size_min_max));
	spa_pod_builder_pop(&b);

	struct spa_fraction rate_min_max[] = { {0, 1}, {INT32_MAX, 1} };
	spa_pod_builder_push_prop(&b,
				  SPA_FORMAT_VIDEO_framerate,
				  SPA_POD_PROP_RANGE_MIN_MAX | SPA_POD_PROP_FLAG_UNSET);
	spa_pod_builder_fraction(&b, 25, 1);
	spa_pod_builder_raw(&b, rate_min_max, sizeof(rate_min_max));
	spa_pod_builder_pop(&b);

	fmt = spa_pod_builder_pop(&b);

	spa_debug_pod(0, NULL, &fmt->pod);

	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	fmt = spa_pod_builder_object(&b,
		SPA_TYPE_OBJECT_Format, 0,
		":", SPA_FORMAT_mediaType,       "I", SPA_MEDIA_TYPE_video,
		":", SPA_FORMAT_mediaSubtype,    "I", SPA_MEDIA_SUBTYPE_raw,
		":", SPA_FORMAT_VIDEO_format,    "Ieu", SPA_VIDEO_FORMAT_I420,
								2, SPA_VIDEO_FORMAT_I420,
								   SPA_VIDEO_FORMAT_YUY2,
		":", SPA_FORMAT_VIDEO_size,      "Rru", &SPA_RECTANGLE(320,241),
								2, &SPA_RECTANGLE(1,1),
								   &SPA_RECTANGLE(INT32_MAX,INT32_MAX),
		":", SPA_FORMAT_VIDEO_framerate, "Fru", &SPA_FRACTION(25,1),
								2, &SPA_FRACTION(0,1),
								   &SPA_FRACTION(INT32_MAX,1));

	spa_debug_pod(0, NULL, &fmt->pod);
	spa_debug_format(0, NULL, &fmt->pod);

	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	/*
	 *  ( "Format",
	 *    ("video", "raw" ),
	 *    {
	 *      "format":    ( "seu", "I420", ( "I420","YUY2" ) ),
	 *      "size":      ( "Rru", (320, 242), ( (1,1), (MAX, MAX)) ),
	 *      "framerate": ( "Fru", (25, 1), ( (0,1), (MAX, 1)) )
	 *    }
	 *  )
	 *
	 *  { "Type" : "Format", "Id" : 0,
	 *    "mediaType":      "video",
	 *    "mediaSubtype":   "raw",
	 *    "videoFormat":      [ "enum", "I420", "YUY2" ],
	 *    "videoSize":        [ "range", [320,242], [1,1], [MAX,MAX] ],
	 *    "videoFramerate":   [ "range", [25,1], [0,1], [MAX,1] ]
	 *  }
	 *
	 */
	fmt = spa_pod_builder_add(&b,
		"{", SPA_TYPE_OBJECT_Format, 0,
		":", SPA_FORMAT_mediaType,       "I", SPA_MEDIA_TYPE_video,
		":", SPA_FORMAT_mediaSubtype,    "I", SPA_MEDIA_SUBTYPE_raw,
		":", SPA_FORMAT_VIDEO_format,    "Ieu", SPA_VIDEO_FORMAT_I420,
								2, SPA_VIDEO_FORMAT_I420,
								   SPA_VIDEO_FORMAT_YUY2,
		":", SPA_FORMAT_VIDEO_size,      "Rru", &SPA_RECTANGLE(320,242),
								2, &SPA_RECTANGLE(1,1),
								   &SPA_RECTANGLE(INT32_MAX,INT32_MAX),
		":", SPA_FORMAT_VIDEO_framerate, "Fru", &SPA_FRACTION(25,1),
								2, &SPA_FRACTION(0,1),
								   &SPA_FRACTION(INT32_MAX,1),
		"}", NULL);

	spa_debug_pod(0, NULL, &fmt->pod);
	spa_debug_format(0, NULL, &fmt->pod);

	do_static_struct();

#if 0
	printf("media type is enum %d\n", spa_type_is_a(SPA_TYPE__mediaType, SPA_TYPE_ENUM_BASE));
	printf("media sybtype is enum %d\n",
	       spa_type_is_a(SPA_TYPE__mediaSubtype, SPA_TYPE_ENUM_BASE));
	printf("format is enum %d\n", spa_type_is_a(SPA_TYPE__Format, SPA_TYPE_ENUM_BASE));
	printf("format is pod %d\n", spa_type_is_a(SPA_TYPE__Format, SPA_TYPE_POD_BASE));
	printf("format is object %d\n", spa_type_is_a(SPA_TYPE__Format, SPA_TYPE_POD_OBJECT_BASE));
#endif

	return 0;
}
