/* PipeWire
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

#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>

#include <pipewire/log.h>
#include <pipewire/transport.h>

/** \cond */

#define INPUT_BUFFER_SIZE       (1<<12)
#define OUTPUT_BUFFER_SIZE      (1<<12)

struct transport {
	struct pw_transport trans;

	struct pw_memblock mem;
	size_t offset;

	struct spa_event current;
	uint32_t current_index;
};
/** \endcond */

static size_t transport_area_get_size(struct pw_transport_area *area)
{
	size_t size;
	size = sizeof(struct pw_transport_area);
	size += area->max_input_ports * sizeof(struct spa_port_io);
	size += area->max_output_ports * sizeof(struct spa_port_io);
	size += sizeof(struct spa_ringbuffer);
	size += INPUT_BUFFER_SIZE;
	size += sizeof(struct spa_ringbuffer);
	size += OUTPUT_BUFFER_SIZE;
	return size;
}

static void transport_setup_area(void *p, struct pw_transport *trans)
{
	struct pw_transport_area *a;

	trans->area = a = p;
	p = SPA_MEMBER(p, sizeof(struct pw_transport_area), struct spa_port_io);

	trans->inputs = p;
	p = SPA_MEMBER(p, a->max_input_ports * sizeof(struct spa_port_io), void);

	trans->outputs = p;
	p = SPA_MEMBER(p, a->max_output_ports * sizeof(struct spa_port_io), void);

	trans->input_buffer = p;
	p = SPA_MEMBER(p, sizeof(struct spa_ringbuffer), void);

	trans->input_data = p;
	p = SPA_MEMBER(p, INPUT_BUFFER_SIZE, void);

	trans->output_buffer = p;
	p = SPA_MEMBER(p, sizeof(struct spa_ringbuffer), void);

	trans->output_data = p;
	p = SPA_MEMBER(p, OUTPUT_BUFFER_SIZE, void);
}

static void transport_reset_area(struct pw_transport *trans)
{
	int i;
	struct pw_transport_area *a = trans->area;

	for (i = 0; i < a->max_input_ports; i++) {
		trans->inputs[i].status = SPA_RESULT_OK;
		trans->inputs[i].buffer_id = SPA_ID_INVALID;
	}
	for (i = 0; i < a->max_output_ports; i++) {
		trans->outputs[i].status = SPA_RESULT_OK;
		trans->outputs[i].buffer_id = SPA_ID_INVALID;
	}
	spa_ringbuffer_init(trans->input_buffer, INPUT_BUFFER_SIZE);
	spa_ringbuffer_init(trans->output_buffer, OUTPUT_BUFFER_SIZE);
}

/** Create a new transport
 * \param max_input_ports maximum number of input_ports
 * \param max_output_ports maximum number of output_ports
 * \return a newly allocated \ref pw_transport
 * \memberof pw_transport
 */
struct pw_transport *pw_transport_new(uint32_t max_input_ports, uint32_t max_output_ports)
{
	struct transport *impl;
	struct pw_transport *trans;
	struct pw_transport_area area;

	area.max_input_ports = max_input_ports;
	area.n_input_ports = 0;
	area.max_output_ports = max_output_ports;
	area.n_output_ports = 0;

	impl = calloc(1, sizeof(struct transport));
	if (impl == NULL)
		return NULL;

	impl->offset = 0;

	trans = &impl->trans;
	pw_signal_init(&trans->destroy_signal);

	pw_memblock_alloc(PW_MEMBLOCK_FLAG_WITH_FD |
			  PW_MEMBLOCK_FLAG_MAP_READWRITE |
			  PW_MEMBLOCK_FLAG_SEAL, transport_area_get_size(&area), &impl->mem);

	memcpy(impl->mem.ptr, &area, sizeof(struct pw_transport_area));
	transport_setup_area(impl->mem.ptr, trans);
	transport_reset_area(trans);

	return trans;
}

struct pw_transport *pw_transport_new_from_info(struct pw_transport_info *info)
{
	struct transport *impl;
	struct pw_transport *trans;
	void *tmp;

	impl = calloc(1, sizeof(struct transport));
	if (impl == NULL)
		return NULL;

	trans = &impl->trans;
	pw_signal_init(&trans->destroy_signal);

	impl->mem.flags = PW_MEMBLOCK_FLAG_MAP_READWRITE | PW_MEMBLOCK_FLAG_WITH_FD;
	impl->mem.fd = info->memfd;
	impl->mem.offset = info->offset;
	impl->mem.size = info->size;
	if (pw_memblock_map(&impl->mem) != SPA_RESULT_OK) {
		pw_log_warn("transport %p: failed to map fd %d: %s", impl, info->memfd,
			    strerror(errno));
		goto mmap_failed;
	}

	impl->offset = info->offset;

	transport_setup_area(impl->mem.ptr, trans);

	tmp = trans->output_buffer;
	trans->output_buffer = trans->input_buffer;
	trans->input_buffer = tmp;

	tmp = trans->output_data;
	trans->output_data = trans->input_data;
	trans->input_data = tmp;

	return trans;

      mmap_failed:
	free(impl);
	return NULL;
}

/** Destroy a transport
 * \param trans a transport to destroy
 * \memberof pw_transport
 */
void pw_transport_destroy(struct pw_transport *trans)
{
	struct transport *impl = (struct transport *) trans;

	pw_log_debug("transport %p: destroy", trans);

	pw_signal_emit(&trans->destroy_signal, trans);

	pw_memblock_free(&impl->mem);
	free(impl);
}

/** Get transport info
 * \param trans the transport to get info of
 * \param[out] info transport info
 * \return 0 on success
 *
 * Fill \a info with the transport info of \a trans. This information can be
 * passed to the client to set up the shared transport.
 *
 * \memberof pw_transport
 */
int pw_transport_get_info(struct pw_transport *trans, struct pw_transport_info *info)
{
	struct transport *impl = (struct transport *) trans;

	info->memfd = impl->mem.fd;
	info->offset = impl->offset;
	info->size = impl->mem.size;

	return SPA_RESULT_OK;
}

/** Add an event to the transport
 * \param trans the transport to send the event on
 * \param event the event to add
 * \return 0 on success, < 0 on error
 *
 * Write \a event to the shared ringbuffer and signal the other side that
 * new data can be read.
 *
 * \memberof pw_transport
 */
int pw_transport_add_event(struct pw_transport *trans, struct spa_event *event)
{
	struct transport *impl = (struct transport *) trans;
	int32_t filled, avail;
	uint32_t size, index;

	if (impl == NULL || event == NULL)
		return SPA_RESULT_INVALID_ARGUMENTS;

	filled = spa_ringbuffer_get_write_index(trans->output_buffer, &index);
	avail = trans->output_buffer->size - filled;
	size = SPA_POD_SIZE(event);
	if (avail < size)
		return SPA_RESULT_ERROR;

	spa_ringbuffer_write_data(trans->output_buffer,
				  trans->output_data,
				  index & trans->output_buffer->mask, event, size);
	spa_ringbuffer_write_update(trans->output_buffer, index + size);

	return SPA_RESULT_OK;
}

/** Get next event from a transport
 * \param trans the transport to get the event of
 * \param[out] event the event to read
 * \return 0 on success, < 0 on error, SPA_RESULT_ENUM_END when no more events
 *	are available.
 *
 * Get the skeleton next event from \a trans into \a event. This function will
 * only read the head and object body of the event.
 *
 * After the complete size of the event has been calculated, you should call
 * \ref pw_transport_parse_event() to read the complete event contents.
 *
 * \memberof pw_transport
 */
int pw_transport_next_event(struct pw_transport *trans, struct spa_event *event)
{
	struct transport *impl = (struct transport *) trans;
	int32_t avail;

	if (impl == NULL || event == NULL)
		return SPA_RESULT_INVALID_ARGUMENTS;

	avail = spa_ringbuffer_get_read_index(trans->input_buffer, &impl->current_index);
	if (avail < sizeof(struct spa_event))
		return SPA_RESULT_ENUM_END;

	spa_ringbuffer_read_data(trans->input_buffer,
				 trans->input_data,
				 impl->current_index & trans->input_buffer->mask,
				 &impl->current, sizeof(struct spa_event));

	*event = impl->current;

	return SPA_RESULT_OK;
}

/** Parse the complete event on transport
 * \param trans the transport to read from
 * \param[out] event memory that can hold the complete event
 * \return 0 on success, < 0 on error
 *
 * Use this function after \ref pw_transport_next_event().
 *
 * \memberof pw_transport
 */
int pw_transport_parse_event(struct pw_transport *trans, void *event)
{
	struct transport *impl = (struct transport *) trans;
	uint32_t size;

	if (impl == NULL || event == NULL)
		return SPA_RESULT_INVALID_ARGUMENTS;

	size = SPA_POD_SIZE(&impl->current);

	spa_ringbuffer_read_data(trans->input_buffer,
				 trans->input_data,
				 impl->current_index & trans->input_buffer->mask, event, size);
	spa_ringbuffer_read_update(trans->input_buffer, impl->current_index + size);

	return SPA_RESULT_OK;
}
