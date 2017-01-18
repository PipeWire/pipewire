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

  SpaRingbufferArea areas[2];
  SpaNodeEvent      current;
} PinosTransportImpl;

static size_t
transport_area_get_size (PinosTransportArea *area)
{
  size_t size;
  size = sizeof (PinosTransportArea);
  size += area->max_inputs * sizeof (SpaPortInput);
  size += area->max_outputs * sizeof (SpaPortOutput);
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
  int i;

  trans->area = a = p;
  p = SPA_MEMBER (p, sizeof (PinosTransportArea), SpaPortInput);

  trans->inputs = p;
  for (i = 0; i < a->max_inputs; i++) {
    trans->inputs[i].state = SPA_PORT_STATE_FLAG_NONE;
    trans->inputs[i].flags = SPA_PORT_INPUT_FLAG_NONE;
    trans->inputs[i].buffer_id = SPA_ID_INVALID;
    trans->inputs[i].status = SPA_RESULT_OK;
  }
  p = SPA_MEMBER (p, a->max_inputs * sizeof (SpaPortInput), void);

  trans->outputs = p;
  for (i = 0; i < a->max_outputs; i++) {
    trans->outputs[i].state = SPA_PORT_STATE_FLAG_NONE;
    trans->outputs[i].flags = SPA_PORT_OUTPUT_FLAG_NONE;
    trans->outputs[i].buffer_id = SPA_ID_INVALID;
    trans->outputs[i].status = SPA_RESULT_OK;
  }
  p = SPA_MEMBER (p, a->max_outputs * sizeof (SpaPortOutput), void);

  trans->input_buffer = p;
  spa_ringbuffer_init (trans->input_buffer, INPUT_BUFFER_SIZE);
  p = SPA_MEMBER (p, sizeof (SpaRingbuffer), void);

  trans->input_data = p;
  p = SPA_MEMBER (p, INPUT_BUFFER_SIZE, void);

  trans->output_buffer = p;
  spa_ringbuffer_init (trans->output_buffer, OUTPUT_BUFFER_SIZE);
  p = SPA_MEMBER (p, sizeof (SpaRingbuffer), void);

  trans->output_data = p;
  p = SPA_MEMBER (p, OUTPUT_BUFFER_SIZE, void);
}

PinosTransport *
pinos_transport_new (unsigned int max_inputs,
                     unsigned int max_outputs)
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
  impl->mem.size = info->size;
  impl->mem.ptr = mmap (NULL, info->size, PROT_READ | PROT_WRITE, MAP_SHARED, info->memfd, info->offset);
  if (impl->mem.ptr == MAP_FAILED) {
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
                           SpaNodeEvent     *event)
{
  PinosTransportImpl *impl = (PinosTransportImpl *) trans;
  SpaRingbufferArea areas[2];
  size_t avail;

  if (impl == NULL || event == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  avail = spa_ringbuffer_get_write_areas (trans->output_buffer, areas);
  if (avail < event->size)
    return SPA_RESULT_ERROR;

  spa_ringbuffer_write_data (trans->output_buffer,
                             trans->output_data,
                             areas,
                             event,
                             event->size);
  spa_ringbuffer_write_advance (trans->output_buffer, event->size);

  return SPA_RESULT_OK;
}

SpaResult
pinos_transport_next_event (PinosTransport *trans,
                            SpaNodeEvent   *event)
{
  PinosTransportImpl *impl = (PinosTransportImpl *) trans;
  size_t avail;

  if (impl == NULL || event == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  avail = spa_ringbuffer_get_read_areas (trans->input_buffer, impl->areas);
  if (avail < sizeof (SpaNodeEvent))
    return SPA_RESULT_ENUM_END;

  spa_ringbuffer_read_data (trans->input_buffer,
                            trans->input_data,
                            impl->areas,
                            &impl->current,
                            sizeof (SpaNodeEvent));

  *event = impl->current;

  return SPA_RESULT_OK;
}

SpaResult
pinos_transport_parse_event (PinosTransport *trans,
                             void           *event)
{
  PinosTransportImpl *impl = (PinosTransportImpl *) trans;

  if (impl == NULL || event == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  spa_ringbuffer_read_data (trans->input_buffer,
                            trans->input_data,
                            impl->areas,
                            event,
                            impl->current.size);
  spa_ringbuffer_read_advance (trans->input_buffer, impl->current.size);

  return SPA_RESULT_OK;
}
