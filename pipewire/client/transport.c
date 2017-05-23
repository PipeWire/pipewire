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

#include <pipewire/client/log.h>
#include <pipewire/client/transport.h>

#define INPUT_BUFFER_SIZE       (1<<12)
#define OUTPUT_BUFFER_SIZE      (1<<12)

#define CMD_NONE                0
#define CMD_PROCESS_DATA       (1<<0)
#define CMD_PROCESS_EVENTS     (1<<1)
#define CMD_PROCESS_SYNC       (1<<2)

struct transport {
  struct pw_transport trans;

  struct pw_memblock  mem;
  size_t              offset;

  SpaEvent            current;
  uint32_t            current_index;
};

static size_t
transport_area_get_size (struct pw_transport_area *area)
{
  size_t size;
  size = sizeof (struct pw_transport_area);
  size += area->max_inputs * sizeof (SpaPortIO);
  size += area->max_outputs * sizeof (SpaPortIO);
  size += sizeof (SpaRingbuffer);
  size += INPUT_BUFFER_SIZE;
  size += sizeof (SpaRingbuffer);
  size += OUTPUT_BUFFER_SIZE;
  return size;
}

static void
transport_setup_area (void *p, struct pw_transport *trans)
{
  struct pw_transport_area *a;

  trans->area = a = p;
  p = SPA_MEMBER (p, sizeof (struct pw_transport_area), SpaPortIO);

  trans->inputs = p;
  p = SPA_MEMBER (p, a->max_inputs * sizeof (SpaPortIO), void);

  trans->outputs = p;
  p = SPA_MEMBER (p, a->max_outputs * sizeof (SpaPortIO), void);

  trans->input_buffer = p;
  p = SPA_MEMBER (p, sizeof (SpaRingbuffer), void);

  trans->input_data = p;
  p = SPA_MEMBER (p, INPUT_BUFFER_SIZE, void);

  trans->output_buffer = p;
  p = SPA_MEMBER (p, sizeof (SpaRingbuffer), void);

  trans->output_data = p;
  p = SPA_MEMBER (p, OUTPUT_BUFFER_SIZE, void);
}

static void
transport_reset_area (struct pw_transport *trans)
{
  int i;
  struct pw_transport_area *a = trans->area;

  for (i = 0; i < a->max_inputs; i++) {
    trans->inputs[i].status = SPA_RESULT_OK;
    trans->inputs[i].buffer_id = SPA_ID_INVALID;
  }
  for (i = 0; i < a->max_outputs; i++) {
    trans->outputs[i].status = SPA_RESULT_OK;
    trans->outputs[i].buffer_id = SPA_ID_INVALID;
  }
  spa_ringbuffer_init (trans->input_buffer, INPUT_BUFFER_SIZE);
  spa_ringbuffer_init (trans->output_buffer, OUTPUT_BUFFER_SIZE);
}

struct pw_transport *
pw_transport_new (uint32_t max_inputs,
                  uint32_t max_outputs)
{
  struct transport *impl;
  struct pw_transport *trans;
  struct pw_transport_area area;

  area.max_inputs = max_inputs;
  area.n_inputs = 0;
  area.max_outputs = max_outputs;
  area.n_outputs = 0;

  impl = calloc (1, sizeof (struct transport));
  if (impl == NULL)
    return NULL;

  impl->offset = 0;

  trans = &impl->trans;
  pw_signal_init (&trans->destroy_signal);

  pw_memblock_alloc (PW_MEMBLOCK_FLAG_WITH_FD |
                        PW_MEMBLOCK_FLAG_MAP_READWRITE |
                        PW_MEMBLOCK_FLAG_SEAL,
                        transport_area_get_size (&area),
                        &impl->mem);

  memcpy (impl->mem.ptr, &area, sizeof (struct pw_transport_area));
  transport_setup_area (impl->mem.ptr, trans);
  transport_reset_area (trans);

  return trans;
}

struct pw_transport *
pw_transport_new_from_info (struct pw_transport_info *info)
{
  struct transport *impl;
  struct pw_transport *trans;
  void *tmp;

  impl = calloc (1, sizeof (struct transport));
  if (impl == NULL)
    return NULL;

  trans = &impl->trans;
  pw_signal_init (&trans->destroy_signal);

  impl->mem.flags = PW_MEMBLOCK_FLAG_MAP_READWRITE |
                    PW_MEMBLOCK_FLAG_WITH_FD;
  impl->mem.fd = info->memfd;
  impl->mem.offset = info->offset;
  impl->mem.size = info->size;
  if (pw_memblock_map (&impl->mem) != SPA_RESULT_OK) {
    pw_log_warn ("transport %p: failed to map fd %d: %s", impl, info->memfd, strerror (errno));
    goto mmap_failed;
  }

  impl->offset = info->offset;

  transport_setup_area (impl->mem.ptr, trans);

  tmp = trans->output_buffer;
  trans->output_buffer = trans->input_buffer;
  trans->input_buffer = tmp;

  tmp = trans->output_data;
  trans->output_data = trans->input_data;
  trans->input_data = tmp;

  return trans;

mmap_failed:
  free (impl);
  return NULL;
}


void
pw_transport_destroy (struct pw_transport *trans)
{
  struct transport *impl = (struct transport *) trans;

  pw_log_debug ("transport %p: destroy", trans);

  pw_signal_emit (&trans->destroy_signal, trans);

  pw_memblock_free (&impl->mem);
  free (impl);
}

SpaResult
pw_transport_get_info (struct pw_transport      *trans,
                       struct pw_transport_info *info)
{
  struct transport *impl = (struct transport *) trans;

  info->memfd = impl->mem.fd;
  info->offset = impl->offset;
  info->size = impl->mem.size;

  return SPA_RESULT_OK;
}

SpaResult
pw_transport_add_event (struct pw_transport *trans,
                        SpaEvent            *event)
{
  struct transport *impl = (struct transport *) trans;
  int32_t filled, avail;
  uint32_t size, index;

  if (impl == NULL || event == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  filled = spa_ringbuffer_get_write_index (trans->output_buffer, &index);
  avail = trans->output_buffer->size - filled;
  size = SPA_POD_SIZE (event);
  if (avail < size)
    return SPA_RESULT_ERROR;

  spa_ringbuffer_write_data (trans->output_buffer,
                             trans->output_data,
                             index & trans->output_buffer->mask,
                             event,
                             size);
  spa_ringbuffer_write_update (trans->output_buffer, index + size);

  return SPA_RESULT_OK;
}

SpaResult
pw_transport_next_event (struct pw_transport *trans,
                         SpaEvent            *event)
{
  struct transport *impl = (struct transport *) trans;
  int32_t avail;

  if (impl == NULL || event == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  avail = spa_ringbuffer_get_read_index (trans->input_buffer, &impl->current_index);
  if (avail < sizeof (SpaEvent))
    return SPA_RESULT_ENUM_END;

  spa_ringbuffer_read_data (trans->input_buffer,
                            trans->input_data,
                            impl->current_index & trans->input_buffer->mask,
                            &impl->current,
                            sizeof (SpaEvent));

  *event = impl->current;

  return SPA_RESULT_OK;
}

SpaResult
pw_transport_parse_event (struct pw_transport *trans,
                          void                *event)
{
  struct transport *impl = (struct transport *) trans;
  uint32_t size;

  if (impl == NULL || event == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  size = SPA_POD_SIZE (&impl->current);

  spa_ringbuffer_read_data (trans->input_buffer,
                            trans->input_data,
                            impl->current_index & trans->input_buffer->mask,
                            event,
                            size);
  spa_ringbuffer_read_update (trans->input_buffer, impl->current_index + size);

  return SPA_RESULT_OK;
}
