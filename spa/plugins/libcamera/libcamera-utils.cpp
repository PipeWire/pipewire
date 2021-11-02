/* Spa
 *
 * Copyright (C) 2020, Collabora Ltd.
 *     Author: Raghavendra Rao Sidlagatta <raghavendra.rao@collabora.com>
 * Copyright (C) 2021 Wim Taymans <wim.taymans@gmail.com>
 *
 * libcamera-utils.cpp
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
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>
#include <errno.h>
#include <sys/mman.h>
#include <poll.h>

#include <linux/media.h>

int spa_libcamera_open(struct impl *impl)
{
	if (impl->acquired)
		return 0;
	impl->camera->acquire();
	impl->acquired = true;
	return 0;
}

int spa_libcamera_close(struct impl *impl)
{
	struct port *port = &impl->out_ports[0];
	if (!impl->acquired)
		return 0;
	if (impl->active || port->have_format)
		return 0;
	impl->camera->release();
	impl->acquired = false;
	return 0;
}

static void spa_libcamera_get_config(struct impl *impl)
{
	if (impl->have_config)
		return;

	StreamRoles roles;
	roles.push_back(VideoRecording);
	impl->config = impl->camera->generateConfiguration(roles);
	impl->have_config = true;
}

static int spa_libcamera_buffer_recycle(struct impl *impl, uint32_t buffer_id)
{
	struct port *port = &impl->out_ports[0];
	struct buffer *b = &port->buffers[buffer_id];

	if (!SPA_FLAG_IS_SET(b->flags, BUFFER_FLAG_OUTSTANDING))
		return 0;

	SPA_FLAG_CLEAR(b->flags, BUFFER_FLAG_OUTSTANDING);
	return 0;
}

static int spa_libcamera_clear_buffers(struct impl *impl)
{
	struct port *port = &impl->out_ports[0];
	uint32_t i;

	if (port->n_buffers == 0)
		return 0;

	for (i = 0; i < port->n_buffers; i++) {
		struct buffer *b;
		struct spa_data *d;

		b = &port->buffers[i];
		d = b->outbuf->datas;

		if (SPA_FLAG_IS_SET(b->flags, BUFFER_FLAG_OUTSTANDING)) {
			spa_log_debug(impl->log, "queueing outstanding buffer %p", b);
			spa_libcamera_buffer_recycle(impl, i);
		}
		if (SPA_FLAG_IS_SET(b->flags, BUFFER_FLAG_MAPPED)) {
			munmap(SPA_PTROFF(b->ptr, -d[0].mapoffset, void),
					d[0].maxsize - d[0].mapoffset);
		}
		if (SPA_FLAG_IS_SET(b->flags, BUFFER_FLAG_ALLOCATED)) {
			close(d[0].fd);
		}
		d[0].type = SPA_ID_INVALID;
	}

	port->n_buffers = 0;

	return 0;
}

struct format_info {
	PixelFormat pix;
	uint32_t format;
	uint32_t media_type;
	uint32_t media_subtype;
};

#define MAKE_FMT(pix,fmt,mt,mst) { pix, SPA_VIDEO_FORMAT_ ##fmt, SPA_MEDIA_TYPE_ ##mt, SPA_MEDIA_SUBTYPE_ ##mst }
static const struct format_info format_info[] = {
	/* RGB formats */
	MAKE_FMT(formats::RGB565, RGB16, video, raw),
	MAKE_FMT(formats::RGB565_BE, RGB16, video, raw),
	MAKE_FMT(formats::RGB888, RGB, video, raw),
	MAKE_FMT(formats::BGR888, BGR, video, raw),
	MAKE_FMT(formats::XRGB8888, xRGB, video, raw),
	MAKE_FMT(formats::XBGR8888, xBGR, video, raw),
	MAKE_FMT(formats::RGBX8888, RGBx, video, raw),
	MAKE_FMT(formats::BGRX8888, BGRx, video, raw),
	MAKE_FMT(formats::ARGB8888, ARGB, video, raw),
	MAKE_FMT(formats::ABGR8888, ABGR, video, raw),
	MAKE_FMT(formats::RGBA8888, RGBA, video, raw),
	MAKE_FMT(formats::BGRA8888, BGRA, video, raw),

	MAKE_FMT(formats::YUYV, YUY2, video, raw),
	MAKE_FMT(formats::YVYU, YVYU, video, raw),
	MAKE_FMT(formats::UYVY, UYVY, video, raw),
	MAKE_FMT(formats::VYUY, VYUY, video, raw),

	MAKE_FMT(formats::NV12, NV12, video, raw),
	MAKE_FMT(formats::NV21, NV21, video, raw),
	MAKE_FMT(formats::NV16, NV16, video, raw),
	MAKE_FMT(formats::NV61, NV61, video, raw),
	MAKE_FMT(formats::NV24, NV24, video, raw),

	MAKE_FMT(formats::YUV420, I420, video, raw),
	MAKE_FMT(formats::YVU420, YV12, video, raw),
	MAKE_FMT(formats::YUV422, Y42B, video, raw),

	MAKE_FMT(formats::MJPEG, ENCODED, video, mjpg),
#undef MAKE_FMT
};

static const struct format_info *video_format_to_info(const PixelFormat &pix) {
	size_t i;

	for (i = 0; i < SPA_N_ELEMENTS(format_info); i++) {
		if (format_info[i].pix == pix)
			return &format_info[i];
	}
	return NULL;
}

static const struct format_info *find_format_info_by_media_type(uint32_t type,
								uint32_t subtype,
								uint32_t format,
								int startidx)
{
	size_t i;

	for (i = startidx; i < SPA_N_ELEMENTS(format_info); i++) {
		if ((format_info[i].media_type == type) &&
		    (format_info[i].media_subtype == subtype) &&
		    (format == 0 || format_info[i].format == format))
			return &format_info[i];
	}
	return NULL;
}

static int
spa_libcamera_enum_format(struct impl *impl, int seq,
		     uint32_t start, uint32_t num,
		     const struct spa_pod *filter)
{
	struct port *port = &impl->out_ports[0];
	int res;
	const struct format_info *info;
	uint8_t buffer[1024];
	struct spa_pod_builder b = { 0 };
	struct spa_pod_frame f[2];
	struct spa_result_node_params result;
	struct spa_pod *fmt;
	uint32_t count = 0;
	PixelFormat format;
	Size frameSize;
	SizeRange sizeRange = SizeRange();

	spa_libcamera_get_config(impl);

	const StreamConfiguration& streamConfig = impl->config->at(0);
	const StreamFormats &formats = streamConfig.formats();

	result.id = SPA_PARAM_EnumFormat;
	result.next = start;

	if (result.next == 0) {
		port->fmt_index = 0;
		port->size_index = 0;
	}
next:
	result.index = result.next++;

next_fmt:
	if (port->fmt_index >= formats.pixelformats().size())
		goto enum_end;

	format = formats.pixelformats()[port->fmt_index];

	spa_log_debug(impl->log, "format: %s", format.toString().c_str());

	info = video_format_to_info(format);
	if (info == NULL) {
		spa_log_debug(impl->log, "unknown format");
		port->fmt_index++;
		goto next_fmt;
	}

	if (port->size_index < formats.sizes(format).size()) {
		frameSize = formats.sizes(format)[port->size_index];
	} else if (port->size_index < 1) {
		sizeRange = formats.range(format);
		if (sizeRange.hStep == 0 || sizeRange.vStep == 0) {
			port->size_index = 0;
			port->fmt_index++;
			goto next_fmt;
		}
	} else {
		port->size_index = 0;
		port->fmt_index++;
		goto next_fmt;
	}
	port->size_index++;

	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	spa_pod_builder_push_object(&b, &f[0], SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
	spa_pod_builder_add(&b,
			SPA_FORMAT_mediaType,    SPA_POD_Id(info->media_type),
			SPA_FORMAT_mediaSubtype, SPA_POD_Id(info->media_subtype),
			0);

	if (info->media_subtype == SPA_MEDIA_SUBTYPE_raw) {
		spa_pod_builder_prop(&b, SPA_FORMAT_VIDEO_format, 0);
		spa_pod_builder_id(&b, info->format);
	}
	if (info->pix.modifier()) {
		spa_pod_builder_prop(&b, SPA_FORMAT_VIDEO_modifier, 0);
		spa_pod_builder_long(&b, info->pix.modifier());
	}
	spa_pod_builder_prop(&b, SPA_FORMAT_VIDEO_size, 0);

	if (sizeRange.hStep != 0 && sizeRange.vStep != 0) {
		spa_pod_builder_push_choice(&b, &f[1], SPA_CHOICE_Step, 0);
		spa_pod_builder_frame(&b, &f[1]);
		spa_pod_builder_rectangle(&b,
				sizeRange.min.width,
				sizeRange.min.height);
		spa_pod_builder_rectangle(&b,
				sizeRange.min.width,
				sizeRange.min.height);
		spa_pod_builder_rectangle(&b,
				sizeRange.max.width,
				sizeRange.max.height);
		spa_pod_builder_rectangle(&b,
				sizeRange.hStep,
				sizeRange.vStep);
		spa_pod_builder_pop(&b, &f[1]);

	} else {
		spa_pod_builder_rectangle(&b, frameSize.width, frameSize.height);
	}

	fmt = (struct spa_pod*) spa_pod_builder_pop(&b, &f[0]);

	if (spa_pod_filter(&b, &result.param, fmt, filter) < 0)
		goto next;

	spa_node_emit_result(&impl->hooks, seq, 0, SPA_RESULT_TYPE_NODE_PARAMS, &result);

	if (++count != num)
		goto next;

      enum_end:
	res = 0;
	return res;
}

static int spa_libcamera_set_format(struct impl *impl, struct spa_video_info *format, bool try_only)
{
	struct port *port = &impl->out_ports[0];
	const struct format_info *info = NULL;
	uint32_t video_format;
	struct spa_rectangle *size = NULL;
	struct spa_fraction *framerate = NULL;
	int res;

	switch (format->media_subtype) {
	case SPA_MEDIA_SUBTYPE_raw:
		video_format = format->info.raw.format;
		size = &format->info.raw.size;
		framerate = &format->info.raw.framerate;
		break;
	case SPA_MEDIA_SUBTYPE_mjpg:
	case SPA_MEDIA_SUBTYPE_jpeg:
		video_format = SPA_VIDEO_FORMAT_ENCODED;
		size = &format->info.mjpg.size;
		framerate = &format->info.mjpg.framerate;
		break;
	case SPA_MEDIA_SUBTYPE_h264:
		video_format = SPA_VIDEO_FORMAT_ENCODED;
		size = &format->info.h264.size;
		framerate = &format->info.h264.framerate;
		break;
	default:
		video_format = SPA_VIDEO_FORMAT_ENCODED;
		break;
	}

	info = find_format_info_by_media_type(format->media_type,
					      format->media_subtype, video_format, 0);
	if (info == NULL || size == NULL || framerate == NULL) {
		spa_log_error(impl->log, "unknown media type %d %d %d", format->media_type,
			      format->media_subtype, video_format);
		return -EINVAL;
	}
	StreamConfiguration& streamConfig = impl->config->at(0);

	streamConfig.pixelFormat = info->pix;
	streamConfig.size.width = size->width;
	streamConfig.size.height = size->height;

	if (impl->config->validate() == CameraConfiguration::Invalid)
		return -EINVAL;

	if ((res = spa_libcamera_open(impl)) < 0)
		return res;

	res = impl->camera->configure(impl->config.get());
	if (res != 0)
		goto error;

	port->have_format = true;

	port->info.change_mask |= SPA_PORT_CHANGE_MASK_FLAGS | SPA_PORT_CHANGE_MASK_RATE;
	port->info.flags = SPA_PORT_FLAG_CAN_ALLOC_BUFFERS |
		SPA_PORT_FLAG_LIVE |
		SPA_PORT_FLAG_PHYSICAL |
		SPA_PORT_FLAG_TERMINAL;
	port->info.rate = SPA_FRACTION(port->rate.num, port->rate.denom);

	return 0;
error:
	spa_libcamera_close(impl);
	return res;

}

static int
spa_libcamera_enum_controls(struct impl *impl, int seq,
		       uint32_t start, uint32_t num,
		       const struct spa_pod *filter)
{
	return -ENOTSUP;
}

static int mmap_read(struct impl *impl)
{
#if 0
	struct port *port = &impl->out_ports[0];
	struct buffer *b = NULL;
	struct spa_data *d = NULL;
	unsigned int sequence = 0;
	struct timeval timestamp;
	int64_t pts;
	struct OutBuf *pOut = NULL;
	struct CamData *pDatas = NULL;
	uint32_t bytesused = 0;

	timestamp.tv_sec = 0;
	timestamp.tv_usec = 0;

	if (impl->camera) {
//		pOut = (struct OutBuf *)libcamera_get_ring_buffer_data(dev->camera);
		if(!pOut) {
			spa_log_debug(impl->log, "Exiting %s as pOut is NULL", __FUNCTION__);
			return -1;
		}
		/* update the read index of the ring buffer */
//		libcamera_ringbuffer_read_update(dev->camera);

		pDatas = pOut->datas;
		if(NULL == pDatas) {
			spa_log_debug(impl->log, "Exiting %s on NULL pointer", __FUNCTION__);
			goto end;
		}

		b = &port->buffers[pOut->bufIdx];
		b->outbuf->n_datas = pOut->n_datas;

		if(NULL == b->outbuf->datas) {
			spa_log_debug(impl->log, "Exiting %s as b->outbuf->datas is NULL", __FUNCTION__);
			goto end;
		}

		for(unsigned int i = 0;  i < pOut->n_datas; ++i) {
			struct CamData *pData = &pDatas[i];
			if(NULL == pData) {
				spa_log_debug(impl->log, "Exiting %s on NULL pointer", __FUNCTION__);
				goto end;
			}
			b->outbuf->datas[i].flags = SPA_DATA_FLAG_READABLE;
			if(port->memtype == SPA_DATA_DmaBuf) {
				b->outbuf->datas[i].fd = pData->fd;
			}
			bytesused = b->outbuf->datas[i].chunk->size = pData->size;
			timestamp = pData->timestamp;
			sequence = pData->sequence;

			b->outbuf->datas[i].mapoffset = 0;
			b->outbuf->datas[i].chunk->offset = 0;
			b->outbuf->datas[i].chunk->flags = 0;
			//b->outbuf->datas[i].chunk->stride = pData->sstride; /* FIXME:: This needs to be appropriately filled */
			b->outbuf->datas[i].maxsize = pData->maxsize;

			spa_log_trace(impl->log,"Spa libcamera Source::%s:: got bufIdx = %d and ndatas = %d",
				__FUNCTION__, pOut->bufIdx, pOut->n_datas);
			spa_log_trace(impl->log," data[%d] --> fd = %ld bytesused = %d sequence = %d",
				i, b->outbuf->datas[i].fd, bytesused, sequence);
		}
	}

	pts = SPA_TIMEVAL_TO_NSEC(&timestamp);

	if (impl->clock) {
		impl->clock->nsec = pts;
		impl->clock->rate = port->rate;
		impl->clock->position = sequence;
		impl->clock->duration = 1;
		impl->clock->delay = 0;
		impl->clock->rate_diff = 1.0;
		impl->clock->next_nsec = pts + 1000000000LL / port->rate.denom;
	}

	if (b->h) {
		b->h->flags = 0;
		b->h->offset = 0;
		b->h->seq = sequence;
		b->h->pts = pts;
		b->h->dts_offset = 0;
	}

	d = b->outbuf->datas;
	d[0].chunk->offset = 0;
	d[0].chunk->size = bytesused;
	d[0].chunk->flags = 0;
	d[0].data = b->ptr;
	spa_log_trace(impl->log,"%s:: b->ptr = %p d[0].data = %p",
				__FUNCTION__, b->ptr, d[0].data);
	spa_list_append(&port->queue, &b->link);
end:
//	libcamera_free_CamData(dev->camera, pDatas);
//	libcamera_free_OutBuf(dev->camera, pOut);
#endif
	return 0;
}

static void libcamera_on_fd_events(struct spa_source *source)
{
	struct impl *impl = (struct impl*) source->data;
	struct spa_io_buffers *io;
	struct port *port = &impl->out_ports[0];
	struct buffer *b;
	uint64_t cnt;

	if (source->rmask & SPA_IO_ERR) {
		struct port *port = &impl->out_ports[0];
		spa_log_error(impl->log, "libcamera %p: error %08x", impl, source->rmask);
		if (port->source.loop)
			spa_loop_remove_source(impl->data_loop, &port->source);
		return;
	}

	if (!(source->rmask & SPA_IO_IN)) {
		spa_log_warn(impl->log, "libcamera %p: spurious wakeup %d", impl, source->rmask);
		return;
	}

	if (spa_system_eventfd_read(impl->system, port->source.fd, &cnt) < 0) {
		spa_log_error(impl->log, "Failed to read on event fd");
		return;
	}

	if (mmap_read(impl) < 0) {
		spa_log_debug(impl->log, "%s:: mmap_read failure", __FUNCTION__);
		return;
	}

	if (spa_list_is_empty(&port->queue)) {
		spa_log_debug(impl->log, "Exiting %s as spa list is empty", __FUNCTION__);
		return;
	}

	io = port->io;
	if (io != NULL && io->status != SPA_STATUS_HAVE_DATA) {
		if (io->buffer_id < port->n_buffers)
			spa_libcamera_buffer_recycle(impl, io->buffer_id);

		b = spa_list_first(&port->queue, struct buffer, link);
		spa_list_remove(&b->link);
		SPA_FLAG_SET(b->flags, BUFFER_FLAG_OUTSTANDING);

		io->buffer_id = b->id;
		io->status = SPA_STATUS_HAVE_DATA;
		spa_log_trace(impl->log, "libcamera %p: now queued %d", impl, b->id);
	}
	spa_node_call_ready(&impl->callbacks, SPA_STATUS_HAVE_DATA);
}

static int spa_libcamera_use_buffers(struct impl *impl, struct spa_buffer **buffers, uint32_t n_buffers)
{
#if 0
	struct port *port = &impl->out_ports[0];
	unsigned int i, j;
	struct spa_data *d;

	n_buffers = libcamera_get_nbuffers(port->dev.camera);
	if (n_buffers > 0) {
		d = buffers[0]->datas;

		if (d[0].type == SPA_DATA_MemFd ||
		    (d[0].type == SPA_DATA_MemPtr && d[0].data != NULL)) {
			port->memtype = SPA_DATA_MemPtr;
		} else if (d[0].type == SPA_DATA_DmaBuf) {
			port->memtype = SPA_DATA_DmaBuf;
		} else {
			spa_log_error(impl->log, "v4l2: can't use buffers of type %d", d[0].type);
			return -EINVAL;
		}
	}

	for (i = 0; i < n_buffers; i++) {
		struct buffer *b;

		b = &port->buffers[i];
		b->id = i;
		b->outbuf = buffers[i];
		b->flags = BUFFER_FLAG_OUTSTANDING;
		b->h = spa_buffer_find_meta_data(buffers[i], SPA_META_Header, sizeof(*b->h));

		spa_log_debug(impl->log, "import buffer %p", buffers[i]);

		if (buffers[i]->n_datas < 1) {
			spa_log_error(impl->log, "invalid memory on buffer %p", buffers[i]);
			return -EINVAL;
		}

		d = buffers[i]->datas;
		for(j = 0; j < buffers[i]->n_datas; ++j) {
			d[j].mapoffset = 0;
			d[j].maxsize = libcamera_get_max_size(port->dev.camera);

			if (port->memtype == SPA_DATA_MemPtr) {
				if (d[j].data == NULL) {
					d[j].fd = -1;
					d[j].data = mmap(NULL,
						    d[j].maxsize + d[j].mapoffset,
						    PROT_READ, MAP_SHARED,
						    libcamera_get_fd(port->dev.camera, i, j),
						    0);
					if (d[j].data == MAP_FAILED) {
						return -errno;
					}

					b->ptr = d[j].data;
					spa_log_debug(impl->log, "In spa_libcamera_use_buffers(). mmap ptr:%p for fd = %ld buffer: #%d",
						d[j].data, d[j].fd, i);
					SPA_FLAG_SET(b->flags, BUFFER_FLAG_MAPPED);
				} else {
					b->ptr = d[j].data;
					spa_log_debug(impl->log, "In spa_libcamera_use_buffers(). b->ptr = %p d[j].maxsize = %d for buffer: #%d",
						d[j].data, d[j].maxsize, i);
				}
				spa_log_debug(impl->log, "In spa_libcamera_use_buffers(). setting b->ptr = %p for buffer: #%d on libcamera",
						b->ptr, i);
			}
			else if (port->memtype == SPA_DATA_DmaBuf) {
				d[j].fd = libcamera_get_fd(port->dev.camera, i, j);
				spa_log_debug(impl->log, "Got fd = %ld for buffer: #%d", d[j].fd, i);
			}
			else {
				spa_log_error(impl->log, "Exiting spa_libcamera_use_buffers() with -EIO");
				return -EIO;
			}
		}

		spa_libcamera_buffer_recycle(impl, i);
	}
	port->n_buffers = n_buffers;

#endif
	return 0;
}

static int
mmap_init(struct impl *impl,
		struct spa_buffer **buffers, uint32_t n_buffers)
{
#if 0
	struct port *port = &impl->out_ports[0];
	unsigned int i, j;
	struct spa_data *d;

	spa_log_info(impl->log, "In mmap_init()");

	if (n_buffers > 0) {
		d = buffers[0]->datas;

		if (d[0].type != SPA_ID_INVALID &&
		    d[0].type & (1u << SPA_DATA_DmaBuf)) {
			port->memtype = SPA_DATA_DmaBuf;
		} else if (d[0].type != SPA_ID_INVALID &&
		    d[0].type & (1u << SPA_DATA_MemFd)) {
			port->memtype = SPA_DATA_MemFd;
		} else if (d[0].type & (1u << SPA_DATA_MemPtr)) {
			port->memtype = SPA_DATA_MemPtr;
		} else {
			spa_log_error(impl->log, "v4l2: can't use buffers of type %d", d[0].type);
			return -EINVAL;
		}
	}

	/* get n_buffers from libcamera */
	uint32_t libcamera_nbuffers = libcamera_get_nbuffers(port->dev.camera);

	for (i = 0; i < libcamera_nbuffers; i++) {
		struct buffer *b;

		if (buffers[i]->n_datas < 1) {
			spa_log_error(impl->log, "invalid buffer data");
			return -EINVAL;
		}

		b = &port->buffers[i];
		b->id = i;
		b->outbuf = buffers[i];
		b->flags = BUFFER_FLAG_OUTSTANDING;
		b->h = spa_buffer_find_meta_data(buffers[i], SPA_META_Header, sizeof(*b->h));

		d = buffers[i]->datas;
		for(j = 0; j < buffers[i]->n_datas; ++j) {
			d[j].type = port->memtype;
			d[j].flags = SPA_DATA_FLAG_READABLE;
			d[j].mapoffset = 0;
			d[j].maxsize = libcamera_get_max_size(port->dev.camera);
			d[j].chunk->offset = 0;
			d[j].chunk->size = 0;
			d[j].chunk->stride = port->fmt.bytesperline; /* FIXME:: This needs to be appropriately filled */
			d[j].chunk->flags = 0;

			if (port->memtype == SPA_DATA_DmaBuf ||
			    port->memtype == SPA_DATA_MemFd) {
				d[j].fd = libcamera_get_fd(port->dev.camera, i, j);
				spa_log_info(impl->log, "Got fd = %ld for buffer: #%d", d[j].fd, i);
				d[j].data = NULL;
				SPA_FLAG_SET(b->flags, BUFFER_FLAG_ALLOCATED);
			}
			else if(port->memtype == SPA_DATA_MemPtr) {
				d[j].fd = -1;
				d[j].data = mmap(NULL,
						    d[j].maxsize + d[j].mapoffset,
						    PROT_READ, MAP_SHARED,
						    libcamera_get_fd(port->dev.camera, i, j),
						    0);
				if (d[j].data == MAP_FAILED) {
					spa_log_error(impl->log, "mmap: %m");
					continue;
				}
				b->ptr = d[j].data;
				SPA_FLAG_SET(b->flags, BUFFER_FLAG_MAPPED);
				spa_log_info(impl->log, "mmap ptr:%p", d[j].data);
			} else {
				spa_log_error(impl->log, "invalid buffer type");
				return -EIO;
			}
		}

		spa_libcamera_buffer_recycle(impl, i);
	}
	port->n_buffers = libcamera_nbuffers;
#endif
	return 0;
}

static int
spa_libcamera_alloc_buffers(struct impl *impl,
		       struct spa_buffer **buffers,
		       uint32_t n_buffers)
{
	int res;
	struct port *port = &impl->out_ports[0];

	if (port->n_buffers > 0)
		return -EIO;

	if ((res = mmap_init(impl, buffers, n_buffers)) < 0) {
		return -EIO;
	}

	return 0;
}

static int spa_libcamera_stream_on(struct impl *impl)
{
	struct port *port = &impl->out_ports[0];

	if (!port->have_format) {
		spa_log_error(impl->log, "Exting %s with -EIO", __FUNCTION__);
		return -EIO;
	}

	if (impl->active)
		return 0;

	spa_log_info(impl->log, "connecting camera");

//	libcamera_connect(dev->camera);

//	libcamera_start_capture(dev->camera);

	port->source.func = libcamera_on_fd_events;
	port->source.data = impl;
	port->source.fd = spa_system_eventfd_create(impl->system, SPA_FD_CLOEXEC | SPA_FD_NONBLOCK);
	port->source.mask = SPA_IO_IN | SPA_IO_ERR;
	port->source.rmask = 0;
	if (port->source.fd < 0) {
		spa_log_error(impl->log, "Failed to create eventfd. Exting %s with -EIO", __FUNCTION__);
	} else {
		spa_loop_add_source(impl->data_loop, &port->source);
		impl->have_source = true;

//		libcamera_set_spa_system(dev->camera, impl->system);
//		libcamera_set_eventfd(dev->camera, port->source.fd);
	}

	impl->active = true;

	return 0;
}

static int do_remove_source(struct spa_loop *loop,
			    bool async,
			    uint32_t seq,
			    const void *data,
			    size_t size,
			    void *user_data)
{
	struct port *port = (struct port *)user_data;
	if (port->source.loop)
		spa_loop_remove_source(loop, &port->source);
	return 0;
}

static int spa_libcamera_stream_off(struct impl *impl)
{
	struct port *port = &impl->out_ports[0];

	if (!impl->active)
		return 0;

	spa_log_info(impl->log, "stopping camera");

//	libcamera_stop_capture(dev->camera);

	spa_log_info(impl->log, "disconnecting camera");

//	libcamera_disconnect(dev->camera);

	spa_loop_invoke(impl->data_loop, do_remove_source, 0, NULL, 0, true, port);

	spa_list_init(&port->queue);
	impl->active = false;

	return 0;
}
