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

#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>

#include <pinos/client/log.h>
#include <pinos/client/transport.h>

#define INPUT_BUFFER_SIZE       (1<<12)
#define OUTPUT_BUFFER_SIZE      (1<<12)

#define CMD_NONE                0
#define CMD_PROCESS_DATA       (1<<0)
#define CMD_PROCESS_EVENTS     (1<<1)
#define CMD_PROCESS_SYNC       (1<<2)

typedef struct {
  PinosTransport trans;

  PinosMemblock  mem;
  size_t         offset;

  SpaEvent          current;
  uint32_t          current_index;
} PinosTransportImpl;

static size_t
transport_area_get_size (PinosTransportArea *area)
{
  size_t size;
  size = sizeof (PinosTransportArea);
  size += area->max_inputs * sizeof (SpaPortIO);
  size += area->max_outputs * sizeof (SpaPortIO);
  size += sizeof (SpaRingbuffer);
  size += INPUT_BUFFER_SIZE;
  size += sizeof (SpaRingbuffer);
  size += OUTPUT_BUFFER_SIZE;
  return size;
}

static void
transport_setup_area (void *p, PinosTransport *trans)
{
  PinosTransportArea *a;

  trans->area = a = p;
  p = SPA_MEMBER (p, sizeof (PinosTransportArea), SpaPortIO);

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
transport_reset_area (PinosTransport *trans)
{
  int i;
  PinosTransportArea *a = trans->area;

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

PinosTransport *
pinos_transport_new (uint32_t max_inputs,
                     uint32_t max_outputs)
{
  PinosTransportImpl *impl;
  PinosTransport *trans;
  PinosTransportArea area;

  area.max_inputs = max_inputs;
  area.n_inputs = 0;
  area.max_outputs = max_outputs;
  area.n_outputs = 0;

  impl = calloc (1, sizeof (PinosTransportImpl));
  if (impl == NULL)
    return NULL;

  impl->offset = 0;

  trans = &impl->trans;
  pinos_signal_init (&trans->destroy_signal);

  pinos_memblock_alloc (PINOS_MEMBLOCK_FLAG_WITH_FD |
                        PINOS_MEMBLOCK_FLAG_MAP_READWRITE |
                        PINOS_MEMBLOCK_FLAG_SEAL,
                        transport_area_get_size (&area),
                        &impl->mem);

  memcpy (impl->mem.ptr, &area, sizeof (PinosTransportArea));
  transport_setup_area (impl->mem.ptr, trans);
  transport_reset_area (trans);

  return trans;
}

PinosTransport *
pinos_transport_new_from_info (PinosTransportInfo *info)
{
  PinosTransportImpl *impl;
  PinosTransport *trans;
  void *tmp;

  impl = calloc (1, sizeof (PinosTransportImpl));
  if (impl == NULL)
    return NULL;

  trans = &impl->trans;
  pinos_signal_init (&trans->destroy_signal);

  impl->mem.flags = PINOS_MEMBLOCK_FLAG_MAP_READWRITE |
                    PINOS_MEMBLOCK_FLAG_WITH_FD;
  impl->mem.fd = info->memfd;
  impl->mem.offset = info->offset;
  impl->mem.size = info->size;
  if (pinos_memblock_map (&impl->mem) != SPA_RESULT_OK) {
    pinos_log_warn ("transport %p: failed to map fd %d: %s", impl, info->memfd, strerror (errno));
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
pinos_transport_destroy (PinosTransport *trans)
{
  PinosTransportImpl *impl = (PinosTransportImpl *) trans;

  pinos_log_debug ("transport %p: destroy", trans);

  pinos_signal_emit (&trans->destroy_signal, trans);

  pinos_memblock_free (&impl->mem);
  free (impl);
}

SpaResult
pinos_transport_get_info (PinosTransport     *trans,
                          PinosTransportInfo *info)
{
  PinosTransportImpl *impl = (PinosTransportImpl *) trans;

  info->memfd = impl->mem.fd;
  info->offset = impl->offset;
  info->size = impl->mem.size;

  return SPA_RESULT_OK;
}

SpaResult
pinos_transport_add_event (PinosTransport   *trans,
                           SpaEvent         *event)
{
  PinosTransportImpl *impl = (PinosTransportImpl *) trans;
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
pinos_transport_next_event (PinosTransport *trans,
                            SpaEvent       *event)
{
  PinosTransportImpl *impl = (PinosTransportImpl *) trans;
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
pinos_transport_parse_event (PinosTransport *trans,
                             void           *event)
{
  PinosTransportImpl *impl = (PinosTransportImpl *) trans;
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
