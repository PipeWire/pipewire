/*
  [title]
  \ref page_tutorial5
  [title]
 */
/* [code] */
#include <spa/param/video/format-utils.h>
#include <spa/debug/types.h>
#include <spa/param/video/type-info.h>

#include <pipewire/pipewire.h>

struct data {
	struct pw_main_loop *loop;
	struct pw_stream *stream;

	struct spa_video_info format;
};

/* [on_process] */
static void on_process(void *userdata)
{
	struct data *data = userdata;
	struct pw_buffer *b;
	struct spa_buffer *buf;

	if ((b = pw_stream_dequeue_buffer(data->stream)) == NULL) {
		pw_log_warn("out of buffers: %m");
		return;
	}

	buf = b->buffer;
	if (buf->datas[0].data == NULL)
		return;

	/** copy frame data to screen */
	printf("got a frame of size %d\n", buf->datas[0].chunk->size);

	pw_stream_queue_buffer(data->stream, b);
}
/* [on_process] */

static void on_param_changed(void *userdata, uint32_t id, const struct spa_pod *param)
{
	struct data *data = userdata;

	if (param == NULL || id != SPA_PARAM_Format)
		return;

	if (spa_format_parse(param,
			&data->format.media_type,
			&data->format.media_subtype) < 0)
		return;

	if (data->format.media_type != SPA_MEDIA_TYPE_video ||
	    data->format.media_subtype != SPA_MEDIA_SUBTYPE_raw)
		return;

	if (spa_format_video_raw_parse(param, &data->format.info.raw) < 0)
		return;

	printf("got video format:\n");
	printf("  format: %d (%s)\n", data->format.info.raw.format,
			spa_debug_type_find_name(spa_type_video_format,
				data->format.info.raw.format));
	printf("  size: %dx%d\n", data->format.info.raw.size.width,
			data->format.info.raw.size.height);
	printf("  framerate: %d/%d\n", data->format.info.raw.framerate.num,
			data->format.info.raw.framerate.denom);

	/** prepare to render video of this size */
}

static const struct pw_stream_events stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.param_changed = on_param_changed,
	.process = on_process,
};

int main(int argc, char *argv[])
{
	struct data data = { 0, };
	const struct spa_pod *params[1];
	uint8_t buffer[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	struct pw_properties *props;

	pw_init(&argc, &argv);

	data.loop = pw_main_loop_new(NULL);

	props = pw_properties_new(PW_KEY_MEDIA_TYPE, "Video",
			PW_KEY_MEDIA_CATEGORY, "Capture",
			PW_KEY_MEDIA_ROLE, "Camera",
			NULL);
	if (argc > 1)
		pw_properties_set(props, PW_KEY_TARGET_OBJECT, argv[1]);

	data.stream = pw_stream_new_simple(
			pw_main_loop_get_loop(data.loop),
			"video-capture",
			props,
			&stream_events,
			&data);

	params[0] = spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
		SPA_FORMAT_mediaType,       SPA_POD_Id(SPA_MEDIA_TYPE_video),
		SPA_FORMAT_mediaSubtype,    SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
		SPA_FORMAT_VIDEO_format,    SPA_POD_CHOICE_ENUM_Id(7,
						SPA_VIDEO_FORMAT_RGB,
						SPA_VIDEO_FORMAT_RGB,
						SPA_VIDEO_FORMAT_RGBA,
						SPA_VIDEO_FORMAT_RGBx,
						SPA_VIDEO_FORMAT_BGRx,
						SPA_VIDEO_FORMAT_YUY2,
						SPA_VIDEO_FORMAT_I420),
		SPA_FORMAT_VIDEO_size,      SPA_POD_CHOICE_RANGE_Rectangle(
						&SPA_RECTANGLE(320, 240),
						&SPA_RECTANGLE(1, 1),
						&SPA_RECTANGLE(4096, 4096)),
		SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(
						&SPA_FRACTION(25, 1),
						&SPA_FRACTION(0, 1),
						&SPA_FRACTION(1000, 1)));

	pw_stream_connect(data.stream,
			  PW_DIRECTION_INPUT,
			  PW_ID_ANY,
			  PW_STREAM_FLAG_AUTOCONNECT |
			  PW_STREAM_FLAG_MAP_BUFFERS,
			  params, 1);

	pw_main_loop_run(data.loop);

	pw_stream_destroy(data.stream);
	pw_main_loop_destroy(data.loop);

	return 0;
}
/* [code] */
