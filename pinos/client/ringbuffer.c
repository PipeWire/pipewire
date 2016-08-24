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

#define _GNU_SOURCE

#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/eventfd.h>
#include <sys/mman.h>

#include <gio/gio.h>
#include <glib-object.h>

#include <pinos/client/enumtypes.h>
#include <pinos/client/ringbuffer.h>

#include <spa/include/spa/ringbuffer.h>
#include <spa/lib/ringbuffer.c>

#define PINOS_RINGBUFFER_GET_PRIVATE(rb)  \
   (G_TYPE_INSTANCE_GET_PRIVATE ((rb), PINOS_TYPE_RINGBUFFER, PinosRingbufferPrivate))

typedef struct {
  SpaRingbuffer rbuf;
  /* ringbuffer starts here */
} PinosRingbufferData;

struct _PinosRingbufferPrivate
{
  PinosRingbufferMode mode;
  guint size;
  guint fdsize;
  int fd;
  int semaphore;

  PinosRingbufferData *data;
};

G_DEFINE_TYPE (PinosRingbuffer, pinos_ringbuffer, G_TYPE_OBJECT);

enum
{
  PROP_0,
  PROP_MODE,
  PROP_SIZE,
  PROP_FD,
  PROP_FDSIZE,
  PROP_SEMAPHORE,
};

enum
{
  SIGNAL_NONE,
  LAST_SIGNAL
};

static void
pinos_ringbuffer_get_property (GObject    *_object,
                                guint       prop_id,
                                GValue     *value,
                                GParamSpec *pspec)
{
  PinosRingbuffer *rb = PINOS_RINGBUFFER (_object);
  PinosRingbufferPrivate *priv = rb->priv;

  switch (prop_id) {
    case PROP_MODE:
      g_value_set_enum (value, priv->mode);
      break;

    case PROP_SIZE:
      g_value_set_uint (value, priv->size);
      break;

    case PROP_FD:
      g_value_set_int (value, priv->fd);
      break;

    case PROP_FDSIZE:
      g_value_set_uint (value, priv->fdsize);
      break;

    case PROP_SEMAPHORE:
      g_value_set_int (value, priv->semaphore);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (rb, prop_id, pspec);
      break;
  }
}

static void
pinos_ringbuffer_set_property (GObject      *_object,
                                guint         prop_id,
                                const GValue *value,
                                GParamSpec   *pspec)
{
  PinosRingbuffer *rb = PINOS_RINGBUFFER (_object);
  PinosRingbufferPrivate *priv = rb->priv;

  switch (prop_id) {
    case PROP_MODE:
      priv->mode = g_value_get_enum (value);
      break;

    case PROP_SIZE:
      priv->size = g_value_get_uint (value);
      break;

    case PROP_FD:
      priv->fd = g_value_get_int (value);
      break;

    case PROP_FDSIZE:
      priv->fdsize = g_value_get_uint (value);
      break;

    case PROP_SEMAPHORE:
      priv->semaphore = g_value_get_int (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (rb, prop_id, pspec);
      break;
  }
}

static int
tmpfile_create (gsize size)
{
  char filename[] = "/dev/shm/tmpfilepay.XXXXXX";
  int fd, result;

  fd = mkostemp (filename, O_CLOEXEC);
  if (fd == -1)
    return -1;
  unlink (filename);

  result = ftruncate (fd, size);
  if (result == -1) {
    close (fd);
    return -1;
  }
  return fd;
}

static void
pinos_ringbuffer_constructed (GObject * obj)
{
  PinosRingbuffer *rb = PINOS_RINGBUFFER (obj);
  PinosRingbufferPrivate *priv = rb->priv;

  g_debug ("ringbuffer %p: constructed", rb);

  if (priv->fd == -1) {
    priv->fdsize = priv->size + sizeof (PinosRingbufferData);
    priv->fd = tmpfile_create (priv->fdsize);
    priv->semaphore = eventfd (0, EFD_CLOEXEC);
  }
  priv->data = mmap (NULL, priv->fdsize, PROT_READ | PROT_WRITE, MAP_SHARED, priv->fd, 0);

  spa_ringbuffer_init (&priv->data->rbuf, (guint8 *)priv->data + sizeof (PinosRingbufferData), priv->size);

  G_OBJECT_CLASS (pinos_ringbuffer_parent_class)->constructed (obj);
}

static void
pinos_ringbuffer_dispose (GObject * obj)
{
  PinosRingbuffer *rb = PINOS_RINGBUFFER (obj);

  g_debug ("ringbuffer %p: dispose", rb);

  G_OBJECT_CLASS (pinos_ringbuffer_parent_class)->dispose (obj);
}

static void
pinos_ringbuffer_finalize (GObject * obj)
{
  PinosRingbuffer *rb = PINOS_RINGBUFFER (obj);

  g_debug ("ringbuffer %p: finalize", rb);

  G_OBJECT_CLASS (pinos_ringbuffer_parent_class)->finalize (obj);
}

static void
pinos_ringbuffer_class_init (PinosRingbufferClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  g_type_class_add_private (klass, sizeof (PinosRingbufferPrivate));

  gobject_class->constructed = pinos_ringbuffer_constructed;
  gobject_class->dispose = pinos_ringbuffer_dispose;
  gobject_class->finalize = pinos_ringbuffer_finalize;
  gobject_class->set_property = pinos_ringbuffer_set_property;
  gobject_class->get_property = pinos_ringbuffer_get_property;

  g_object_class_install_property (gobject_class,
                                   PROP_MODE,
                                   g_param_spec_enum ("mode",
                                                      "Mode",
                                                      "The mode of the ringbuffer",
                                                      PINOS_TYPE_RINGBUFFER_MODE,
                                                      PINOS_RINGBUFFER_MODE_READ,
                                                      G_PARAM_READWRITE |
                                                      G_PARAM_CONSTRUCT_ONLY |
                                                      G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
                                   PROP_SIZE,
                                   g_param_spec_uint ("size",
                                                      "Size",
                                                      "The size of the ringbuffer",
                                                      1,
                                                      G_MAXUINT,
                                                      64 * 1024,
                                                      G_PARAM_READWRITE |
                                                      G_PARAM_CONSTRUCT_ONLY |
                                                      G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
                                   PROP_FD,
                                   g_param_spec_int ("fd",
                                                     "Fd",
                                                     "The file descriptor with memory",
                                                     -1,
                                                     G_MAXINT,
                                                     -1,
                                                     G_PARAM_READWRITE |
                                                     G_PARAM_CONSTRUCT_ONLY |
                                                     G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
                                   PROP_FDSIZE,
                                   g_param_spec_uint ("fdsize",
                                                      "Fd Size",
                                                      "Size of the memory",
                                                      1,
                                                      G_MAXUINT,
                                                      -1,
                                                      G_PARAM_READWRITE |
                                                      G_PARAM_CONSTRUCT_ONLY |
                                                      G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
                                   PROP_SEMAPHORE,
                                   g_param_spec_int ("semaphore",
                                                     "Semaphore",
                                                     "Semaphore file desciptor",
                                                     -1,
                                                     G_MAXINT,
                                                     -1,
                                                     G_PARAM_READWRITE |
                                                     G_PARAM_CONSTRUCT_ONLY |
                                                     G_PARAM_STATIC_STRINGS));
}

static void
pinos_ringbuffer_init (PinosRingbuffer * rb)
{
  PinosRingbufferPrivate *priv = rb->priv = PINOS_RINGBUFFER_GET_PRIVATE (rb);

  g_debug ("ringbuffer %p: new %u", rb, priv->size);

  priv->mode = PINOS_RINGBUFFER_MODE_READ;
  priv->size = 0;
  priv->fd = -1;
}

PinosRingbuffer *
pinos_ringbuffer_new (PinosRingbufferMode  mode,
                      gsize                size)
{
  PinosRingbuffer *rb;

  g_return_val_if_fail (size > 0, NULL);

  rb = g_object_new (PINOS_TYPE_RINGBUFFER,
                     "size", size,
                     "mode", mode,
                     NULL);
  return rb;
}

PinosRingbuffer *
pinos_ringbuffer_new_import (PinosRingbufferMode  mode,
                             guint                fdsize,
                             int                  fd,
                             int                  semaphore)
{
  PinosRingbuffer *rb;

  g_return_val_if_fail (fdsize > 0, NULL);
  g_return_val_if_fail (fd >= 0, NULL);

  rb = g_object_new (PINOS_TYPE_RINGBUFFER,
                     "mode", mode,
                     "fd", fd,
                     "fdsize", fdsize,
                     "semaphore", semaphore,
                     NULL);
  return rb;
}


gboolean
pinos_ringbuffer_get_read_areas (PinosRingbuffer      *rbuf,
                                 PinosRingbufferArea   areas[2])
{
  PinosRingbufferPrivate *priv;

  g_return_val_if_fail (PINOS_IS_RINGBUFFER (rbuf), FALSE);
  priv = rbuf->priv;

  spa_ringbuffer_get_read_areas (&priv->data->rbuf, (SpaRingbufferArea *)areas);

  return TRUE;
}

gboolean
pinos_ringbuffer_get_write_areas (PinosRingbuffer      *rbuf,
                                  PinosRingbufferArea   areas[2])
{
  PinosRingbufferPrivate *priv;

  g_return_val_if_fail (PINOS_IS_RINGBUFFER (rbuf), FALSE);
  priv = rbuf->priv;

  spa_ringbuffer_get_write_areas (&priv->data->rbuf, (SpaRingbufferArea *)areas);

  return TRUE;
}

gboolean
pinos_ringbuffer_read_advance (PinosRingbuffer      *rbuf,
                               gssize                len)
{
  PinosRingbufferPrivate *priv;
  guint64 val;

  g_return_val_if_fail (PINOS_IS_RINGBUFFER (rbuf), FALSE);
  priv = rbuf->priv;

  spa_ringbuffer_read_advance (&priv->data->rbuf, len);

  if (priv->mode == PINOS_RINGBUFFER_MODE_READ) {
    val = 1;
    if (write (priv->semaphore, &val, 8) != 8)
      g_warning ("error writing semaphore");
  }

  return TRUE;
}

gboolean
pinos_ringbuffer_write_advance (PinosRingbuffer      *rbuf,
                                gssize                len)
{
  PinosRingbufferPrivate *priv;
  guint64 val;

  g_return_val_if_fail (PINOS_IS_RINGBUFFER (rbuf), FALSE);
  priv = rbuf->priv;

  spa_ringbuffer_write_advance (&priv->data->rbuf, len);

  if (priv->mode == PINOS_RINGBUFFER_MODE_WRITE) {
    val = 1;
    if (write (priv->semaphore, &val, 8) != 8)
      g_warning ("error writing semaphore");
  }
  return TRUE;
}
