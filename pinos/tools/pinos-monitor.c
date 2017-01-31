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
  PinosListener on_subscription;
} Data;

static void
print_properties (SpaDict *props, char mark)
{
  SpaDictItem *item;

  if (props == NULL)
    return;

  printf ("%c\tproperties:\n", mark);
  spa_dict_for_each (item, props) {
    printf ("%c\t\t%s = \"%s\"\n", mark, item->key, item->value);
  }
}

typedef struct {
  bool print_mark;
  bool print_all;
} DumpData;

#define MARK_CHANGE(f) ((data->print_mark && ((info)->change_mask & (1 << (f)))) ? '*' : ' ')

static void
dump_core_info (PinosContext        *c,
                SpaResult            res,
                const PinosCoreInfo *info,
                void                *user_data)
{
  DumpData *data = user_data;

  if (info == NULL)
    return;

  printf ("\tid: %u\n", info->id);
  printf ("\ttype: %s\n", PINOS_CORE_URI);
  if (data->print_all) {
    printf ("%c\tuser-name: \"%s\"\n", MARK_CHANGE (0), info->user_name);
    printf ("%c\thost-name: \"%s\"\n", MARK_CHANGE (1), info->host_name);
    printf ("%c\tversion: \"%s\"\n", MARK_CHANGE (2), info->version);
    printf ("%c\tname: \"%s\"\n", MARK_CHANGE (3), info->name);
    printf ("%c\tcookie: %u\n", MARK_CHANGE (4), info->cookie);
    print_properties (info->props, MARK_CHANGE (5));
  }
}

static void
dump_client_info (PinosContext          *c,
                  SpaResult              res,
                  const PinosClientInfo *info,
                  void                  *user_data)
{
  DumpData *data = user_data;

  if (info == NULL)
    return;

  printf ("\tid: %u\n", info->id);
  printf ("\ttype: %s\n", PINOS_CLIENT_URI);
  if (data->print_all) {
    print_properties (info->props, MARK_CHANGE (0));
  }
}

static void
dump_node_info (PinosContext        *c,
                SpaResult            res,
                const PinosNodeInfo *info,
                void                *user_data)
{
  DumpData *data = user_data;

  if (info == NULL) {
    if (res != SPA_RESULT_ENUM_END)
      printf ("\tError introspecting node: %d\n", res);
    return;
  }

  printf ("\tid: %u\n", info->id);
  printf ("\ttype: %s\n", PINOS_NODE_URI);
  if (data->print_all) {
    printf ("%c\tname: \"%s\"\n", MARK_CHANGE (0), info->name);
    printf ("%c\tinputs: %u/%u\n", MARK_CHANGE (1), info->n_inputs, info->max_inputs);
    printf ("%c\toutputs: %u/%u\n", MARK_CHANGE (2), info->n_outputs, info->max_outputs);
    printf ("%c\tstate: \"%s\"", MARK_CHANGE (3), pinos_node_state_as_string (info->state));
    if (info->state == PINOS_NODE_STATE_ERROR && info->error)
      printf (" \"%s\"\n", info->error);
    else
      printf ("\n");
    print_properties (info->props, MARK_CHANGE (4));
  }
}

static void
dump_module_info (PinosContext          *c,
                  SpaResult              res,
                  const PinosModuleInfo *info,
                  void                  *user_data)
{
  DumpData *data = user_data;

  if (info == NULL) {
    if (res != SPA_RESULT_ENUM_END)
      printf ("\tError introspecting module: %d\n", res);
    return;
  }

  printf ("\tid: %u\n", info->id);
  printf ("\ttype: %s\n", PINOS_MODULE_URI);
  if (data->print_all) {
    printf ("%c\tname: \"%s\"\n", MARK_CHANGE (0), info->name);
    printf ("%c\tfilename: \"%s\"\n", MARK_CHANGE (1), info->filename);
    printf ("%c\targs: \"%s\"\n", MARK_CHANGE (2), info->args);
    print_properties (info->props, MARK_CHANGE (3));
  }
}

static void
dump_link_info (PinosContext        *c,
                SpaResult            res,
                const PinosLinkInfo *info,
                void                *user_data)
{
  DumpData *data = user_data;

  if (info == NULL) {
    if (res != SPA_RESULT_ENUM_END)
      printf ("\tError introspecting link: %d\n", res);
    return;
  }

  printf ("\tid: %u\n", info->id);
  printf ("\ttype: %s\n", PINOS_LINK_URI);
  if (data->print_all) {
    printf ("%c\toutput-node-id: %u\n", MARK_CHANGE (0), info->output_node_id);
    printf ("%c\toutput-port-id: %u\n", MARK_CHANGE (1), info->output_port_id);
    printf ("%c\tinput-node-id: %u\n", MARK_CHANGE (2), info->input_node_id);
    printf ("%c\tinput-port-id: %u\n", MARK_CHANGE (3), info->input_port_id);
  }
}

static void
dump_object (PinosContext           *context,
             uint32_t                type,
             uint32_t                id,
             DumpData               *data)
{
  if (type == context->uri.core) {
    pinos_context_get_core_info (context,
                                 dump_core_info,
                                 data);
  } else if (type == context->uri.node) {
    pinos_context_get_node_info_by_id (context,
                                       id,
                                       dump_node_info,
                                       data);
  } else if (type == context->uri.module) {
    pinos_context_get_module_info_by_id (context,
                                         id,
                                         dump_module_info,
                                         data);
  } else if (type == context->uri.client) {
    pinos_context_get_client_info_by_id (context,
                                         id,
                                         dump_client_info,
                                         data);
  } else if (type == context->uri.link) {
    pinos_context_get_link_info_by_id (context,
                                       id,
                                       dump_link_info,
                                       data);
  } else {
    printf ("\tid: %u\n", id);
  }


}

static void
on_subscription (PinosListener          *listener,
                 PinosContext           *context,
                 PinosSubscriptionEvent  event,
                 uint32_t                type,
                 uint32_t                id)
{
  DumpData dd;

  switch (event) {
    case PINOS_SUBSCRIPTION_EVENT_NEW:
      printf ("added:\n");
      dd.print_mark = false;
      dd.print_all = true;
      dump_object (context, type, id, &dd);
      break;

    case PINOS_SUBSCRIPTION_EVENT_CHANGE:
      printf ("changed:\n");
      dd.print_mark = true;
      dd.print_all = true;
      dump_object (context, type, id, &dd);
      break;

    case PINOS_SUBSCRIPTION_EVENT_REMOVE:
      printf ("removed:\n");
      dd.print_mark = false;
      dd.print_all = false;
      dump_object (context, type, id, &dd);
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

  pinos_signal_add (&data.context->subscription,
                    &data.on_subscription,
                    on_subscription);

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
