/* GStreamer */
/* SPDX-FileCopyrightText: Copyright © 2020 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef __GST_PIPEWIRE_CORE_H__
#define __GST_PIPEWIRE_CORE_H__

#include <gst/gst.h>

#include <pipewire/pipewire.h>

G_BEGIN_DECLS

typedef struct _GstPipeWireCore GstPipeWireCore;

#define GST_PIPEWIRE_DEFAULT_TIMEOUT 30

/**
 * GstPipeWireCore:
 *
 * Opaque data structure.
 */
struct _GstPipeWireCore {
  gint refcount;
  int fd;
  struct pw_thread_loop *loop;
  struct pw_context *context;
  struct pw_core *core;
  struct spa_hook core_listener;
  int last_error;
  int last_seq;
  int pending_seq;
};

GstPipeWireCore *gst_pipewire_core_get     (int fd);
void             gst_pipewire_core_release (GstPipeWireCore *core);

G_END_DECLS

#endif /* __GST_PIPEWIRE_CORE_H__ */
