/* Pinos
 * Copyright (C) 2015 Wim Taymans <wim.taymans@gmail.com>
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

#include <stdio.h>

#include <pinos/client/pinos.h>
#include <pinos/client/sig.h>

typedef struct {
  bool running;
  PinosLoop *loop;
  PinosContext *context;

  PinosListener on_state_changed;
} Data;

static void
print_properties (PinosProperties *props, char mark)
{
  void *state = NULL;
  const char *key;

  if (props == NULL)
    return;

  printf ("%c\tproperties:\n", mark);
  while ((key = pinos_properties_iterate (props, &state))) {
    printf ("%c\t\t%s = \"%s\"\n", mark, key, pinos_properties_get (props, key));
  }
}

typedef struct {
  bool print_mark;
  bool print_all;
} DumpData;

#define MARK_CHANGE(f) ((data->print_mark && ((info)->change_mask & (1 << (f)))) ? '*' : ' ')

static void
dump_daemon_info (PinosContext *c, const PinosDaemonInfo *info, void * user_data)
{
  DumpData *data = user_data;

  printf ("\tid: %u\n", info->id);
  if (data->print_all) {
    printf ("%c\tuser-name: \"%s\"\n", MARK_CHANGE (0), info->user_name);
    printf ("%c\thost-name: \"%s\"\n", MARK_CHANGE (1), info->host_name);
    printf ("%c\tversion: \"%s\"\n", MARK_CHANGE (2), info->version);
    printf ("%c\tname: \"%s\"\n", MARK_CHANGE (3), info->name);
    printf ("%c\tcookie: %u\n", MARK_CHANGE (4), info->cookie);
    print_properties (info->properties, MARK_CHANGE (5));
  }
}

static void
dump_client_info (PinosContext *c, const PinosClientInfo *info, void * user_data)
{
  DumpData *data = user_data;

  printf ("\tid: %u\n", info->id);
  if (data->print_all) {
    print_properties (info->properties, MARK_CHANGE (0));
  }
}

static void
dump_node_info (PinosContext *c, const PinosNodeInfo *info, void * user_data)
{
  DumpData *data = user_data;

  printf ("\tid: %u\n", info->id);
  if (data->print_all) {
    printf ("%c\tname: \"%s\"\n", MARK_CHANGE (0), info->name);
    print_properties (info->properties, MARK_CHANGE (1));
    printf ("%c\tstate: \"%s\"\n", MARK_CHANGE (2), pinos_node_state_as_string (info->state));
  }
}

static void
dump_link_info (PinosContext *c, const PinosLinkInfo *info, void * user_data)
{
  DumpData *data = user_data;

  printf ("\tid: %u\n", info->id);
  if (data->print_all) {
    printf ("%c\toutput-node-id: %u\n", MARK_CHANGE (0), info->output_node_id);
    printf ("%c\toutput-port-id: %u\n", MARK_CHANGE (1), info->output_port_id);
    printf ("%c\tinput-node-id: %u\n", MARK_CHANGE (2), info->input_node_id);
    printf ("%c\tinput-port-id: %u\n", MARK_CHANGE (3), info->input_port_id);
  }
}

static void
dump_object (PinosContext           *context,
             uint32_t                id,
             PinosSubscriptionFlags  flags,
             DumpData               *data)
{
  if (flags & PINOS_SUBSCRIPTION_FLAG_DAEMON) {
  }
  else if (flags & PINOS_SUBSCRIPTION_FLAG_CLIENT) {
  }
  else if (flags & PINOS_SUBSCRIPTION_FLAG_NODE) {
  }
  else if (flags & PINOS_SUBSCRIPTION_FLAG_LINK) {
  }
}

static void
subscription_cb (PinosContext           *context,
                 PinosSubscriptionFlags  flags,
                 PinosSubscriptionEvent  type,
                 uint32_t                id,
                 void                   *data)
{
  DumpData dd;

  switch (type) {
    case PINOS_SUBSCRIPTION_EVENT_NEW:
      printf ("added:\n");
      dd.print_mark = false;
      dd.print_all = true;
      dump_object (context, id, flags, &dd);
      break;

    case PINOS_SUBSCRIPTION_EVENT_CHANGE:
      printf ("changed:\n");
      dd.print_mark = true;
      dd.print_all = true;
      dump_object (context, id, flags, &dd);
      break;

    case PINOS_SUBSCRIPTION_EVENT_REMOVE:
      printf ("removed:\n");
      dd.print_mark = false;
      dd.print_all = false;
      dump_object (context, id, flags, &dd);
      break;
  }
}

static void
on_state_changed (PinosListener  *listener,
                  PinosContext   *context)
{
  Data *data = SPA_CONTAINER_OF (listener, Data, on_state_changed);

  switch (context->state) {
    case PINOS_CONTEXT_STATE_ERROR:
      printf ("context error: %s\n", context->error);
      data->running = false;
      break;

    default:
      printf ("context state: \"%s\"\n", pinos_context_state_as_string (context->state));
      break;
  }
}

int
main (int argc, char *argv[])
{
  Data data;

  pinos_init (&argc, &argv);

  data.loop = pinos_loop_new ();
  data.running = true;
  data.context = pinos_context_new (data.loop, "pinos-monitor", NULL);

  pinos_signal_add (&data.context->state_changed,
                    &data.on_state_changed,
                    on_state_changed);

  pinos_context_subscribe (data.context,
                           PINOS_SUBSCRIPTION_FLAGS_ALL,
                           subscription_cb,
                           &data);

  pinos_context_connect (data.context);

  pinos_loop_enter (data.loop);
  while (data.running) {
    pinos_loop_iterate (data.loop, -1);
  }
  pinos_loop_leave (data.loop);

  pinos_context_destroy (data.context);
  pinos_loop_destroy (data.loop);

  return 0;
}
