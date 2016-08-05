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

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>

#include <spa/control.h>
#include <spa/debug.h>

#if 0
#define SPA_DEBUG_CONTROL(format,args...) fprintf(stderr,format,##args)
#else
#define SPA_DEBUG_CONTROL(format,args...)
#endif

typedef struct {
  uint32_t version;
  uint32_t flags;
  uint32_t length;
} SpaStackHeader;

typedef struct {
  void   *data;
  size_t  size;
  size_t  max_size;
  void   *free_data;
  int    *fds;
  int     n_fds;
  int     max_fds;
  void   *free_fds;
  size_t  magic;
} SpaStackControl;

#define SSC(c)              ((SpaStackControl *) (c))
#define SSC_MAGIC           ((size_t) 5493683301u)
#define is_valid_control(c) (c != NULL && \
                             SSC(c)->magic == SSC_MAGIC)

/**
 * spa_control_init_data:
 * @control: a #SpaControl
 * @data: data
 * @size: size of @data
 * @fds: file descriptors
 * @n_fds: number of file descriptors
 *
 * Initialize @control with @data and @size and @fds and @n_fds.
 * The memory pointer to by @data and @fds becomes property of @control
 * and should not be freed or modified until all references to the control
 * are gone.
 */
SpaResult
spa_control_init_data (SpaControl       *control,
                       void              *data,
                       size_t             size,
                       int               *fds,
                       unsigned int       n_fds)
{
  SpaStackControl *sc = SSC (control);

  SPA_DEBUG_CONTROL ("control %p: init", control);

  sc->magic = SSC_MAGIC;
  sc->data = data;
  sc->size = size;
  sc->max_size = size;
  sc->free_data = NULL;
  sc->fds = fds;
  sc->n_fds = n_fds;
  sc->max_fds = n_fds;
  sc->free_fds = NULL;

  return SPA_RESULT_OK;
}

/**
 * spa_control_get_version
 * @control: a #SpaControl
 *
 * Get the control version
 *
 * Returns: the control version.
 */
uint32_t
spa_control_get_version (SpaControl *control)
{
  SpaStackControl *sc = SSC (control);
  SpaStackHeader *hdr;

  if (!is_valid_control (control))
    return -1;

  hdr = sc->data;
  return hdr->version;
}

/**
 * spa_control_get_fd:
 * @control: a #SpaControl
 * @index: an index
 * @steal: steal the fd
 *
 * Get the file descriptor at @index in @control.
 *
 * Returns: a file descriptor at @index in @control. The file descriptor
 * is not duplicated in any way. -1 is returned on error.
 */
int
spa_control_get_fd (SpaControl   *control,
                    unsigned int  index,
                    bool          close)
{
  SpaStackControl *sc = SSC (control);
  int fd;

  if (!is_valid_control (control))
    return -1;

  if (sc->fds == NULL || sc->n_fds < index)
    return -1;

  fd = sc->fds[index];
  if (fd < 0)
    fd = -fd;
  sc->fds[index] = close ? fd : -fd;

  return fd;
}

SpaResult
spa_control_clear (SpaControl *control)
{
  SpaStackControl *sc = SSC (control);
  int i;

  if (!is_valid_control (control))
    return SPA_RESULT_INVALID_ARGUMENTS;

  sc->magic = 0;
  free (sc->free_data);
  for (i = 0; i < sc->n_fds; i++) {
    if (sc->fds[i] > 0) {
      if (close (sc->fds[i]) < 0)
        perror ("close");
    }
  }
  free (sc->free_fds);
  sc->n_fds = 0;

  return SPA_RESULT_OK;
}


/**
 * SpaControlIter:
 *
 * #SpaControlIter is an opaque data structure and can only be accessed
 * using the following functions.
 */
struct stack_iter {
  size_t           magic;
  uint32_t         version;
  SpaStackControl *control;
  size_t           offset;

  SpaControlCmd    cmd;
  size_t           size;
  void            *data;
};

#define SCSI(i)             ((struct stack_iter *) (i))
#define SCSI_MAGIC          ((size_t) 6739527471u)
#define is_valid_iter(i)    (i != NULL && \
                             SCSI(i)->magic == SCSI_MAGIC)

/**
 * spa_control_iter_init:
 * @iter: a #SpaControlIter
 * @control: a #SpaControl
 *
 * Initialize @iter to iterate the packets in @control.
 */
SpaResult
spa_control_iter_init_full (SpaControlIter *iter,
                            SpaControl     *control,
                            uint32_t        version)
{
  struct stack_iter *si = SCSI (iter);

  if (iter == NULL || !is_valid_control (control))
    return SPA_RESULT_INVALID_ARGUMENTS;

  si->magic = SCSI_MAGIC;
  si->version = version;
  si->control = SSC (control);
  si->offset = 0;
  si->cmd = SPA_CONTROL_CMD_INVALID;
  si->size = sizeof (SpaStackHeader);
  si->data = NULL;

  return SPA_RESULT_OK;
}

static bool
read_length (uint8_t * data, unsigned int size, size_t * length, size_t * skip)
{
  size_t len, offset;
  uint8_t b;

  /* start reading the length, we need this to skip to the data later */
  len = offset = 0;
  do {
    if (offset >= size)
      return false;

    b = data[offset++];
    len = (len << 7) | (b & 0x7f);
  } while (b & 0x80);

  /* check remaining control size */
  if (size - offset < len)
    return false;

  *length = len;
  *skip = offset;

  return true;
}

/**
 * spa_control_iter_next:
 * @iter: a #SpaControlIter
 *
 * Move to the next packet in @iter.
 *
 * Returns: %SPA_RESULT_OK if more packets are available.
 */
SpaResult
spa_control_iter_next (SpaControlIter *iter)
{
  struct stack_iter *si = SCSI (iter);
  size_t len, size, skip;
  uint8_t *data;

  if (!is_valid_iter (iter))
    return SPA_RESULT_INVALID_ARGUMENTS;

  /* move to next packet */
  si->offset += si->size;

  /* now read packet */
  data = si->control->data;
  size = si->control->size;
  if (si->offset >= size)
    return SPA_RESULT_ERROR;

  data += si->offset;
  size -= si->offset;

  if (size < 1)
    return SPA_RESULT_ERROR;

  si->cmd = *data;

  data++;
  size--;

  if (!read_length (data, size, &len, &skip))
    return SPA_RESULT_ERROR;

  si->size = len;
  si->data = data + skip;
  si->offset += 1 + skip;

  return SPA_RESULT_OK;
}

/**
 * spa_control_iter_done:
 * @iter: a #SpaControlIter
 *
 * End iterations on @iter.
 */
SpaResult
spa_control_iter_end (SpaControlIter *iter)
{
  struct stack_iter *si = SCSI (iter);

  if (!is_valid_iter (iter))
    return SPA_RESULT_INVALID_ARGUMENTS;

  si->magic = 0;

  return SPA_RESULT_OK;
}

SpaControlCmd
spa_control_iter_get_cmd (SpaControlIter *iter)
{
  struct stack_iter *si = SCSI (iter);

  if (!is_valid_iter (iter))
    return SPA_CONTROL_CMD_INVALID;

  return si->cmd;
}

void *
spa_control_iter_get_data (SpaControlIter *iter, size_t *size)
{
  struct stack_iter *si = SCSI (iter);

  if (!is_valid_iter (iter))
    return NULL;

  if (size)
    *size = si->size;

  return si->data;
}

static void
iter_parse_set_format (struct stack_iter *si, SpaControlCmdSetFormat *cmd)
{
  uint32_t *p = si->data;
  SpaProps *tp;
  unsigned int i, j;
  SpaPropInfo *pi;
  SpaPropRangeInfo *ri;
  SpaMemory *mem;

  cmd->port_id = *p++;
  mem = spa_memory_alloc_size (SPA_MEMORY_POOL_LOCAL, p, si->size - 4);
  cmd->format = spa_memory_ensure_ptr (mem);
  cmd->format->mem.mem = mem->mem;
  cmd->format->mem.offset = 0;
  cmd->format->mem.size = mem->size;

  tp = (SpaProps *) &cmd->format->props;
  tp->prop_info = SPA_MEMBER (tp, SPA_PTR_TO_INT (tp->prop_info), SpaPropInfo);

  /* now fix all the pointers */
  for (i = 0; i < tp->n_prop_info; i++) {
    pi = (SpaPropInfo *) &tp->prop_info[i];
    if (pi->name)
      pi->name = SPA_MEMBER (tp, SPA_PTR_TO_INT (pi->name), char);
    if (pi->description)
      pi->description = SPA_MEMBER (tp, SPA_PTR_TO_INT (pi->description), char);
    if (pi->default_value)
      pi->default_value = SPA_MEMBER (tp, SPA_PTR_TO_INT (pi->default_value), void);
    if (pi->range_values)
      pi->range_values = SPA_MEMBER (tp, SPA_PTR_TO_INT (pi->range_values), SpaPropRangeInfo);

    for (j = 0; j < pi->n_range_values; j++) {
      ri = (SpaPropRangeInfo *) &pi->range_values[j];
      if (ri->name)
        ri->name = SPA_MEMBER (tp, SPA_PTR_TO_INT (ri->name), char);
      if (ri->description)
        ri->description = SPA_MEMBER (tp, SPA_PTR_TO_INT (ri->description), char);
      if (ri->value)
        ri->value = SPA_MEMBER (tp, SPA_PTR_TO_INT (ri->value), void);
    }
  }
}


SpaResult
spa_control_iter_parse_cmd (SpaControlIter *iter,
                            void           *command)
{
  struct stack_iter *si = SCSI (iter);
  SpaResult res = SPA_RESULT_OK;

  if (!is_valid_iter (iter))
    return SPA_RESULT_INVALID_ARGUMENTS;

  switch (si->cmd) {
    /* C -> S */
    case SPA_CONTROL_CMD_NODE_UPDATE:
    case SPA_CONTROL_CMD_PORT_UPDATE:
      fprintf (stderr, "implement iter of %d\n", si->cmd);
      break;

    case SPA_CONTROL_CMD_PORT_REMOVED:
      if (si->size < sizeof (SpaControlCmdPortRemoved))
        return SPA_RESULT_ERROR;
      memcpy (command, si->data, sizeof (SpaControlCmdPortRemoved));
      break;

    case SPA_CONTROL_CMD_STATE_CHANGE:
      if (si->size < sizeof (SpaControlCmdStateChange))
        return SPA_RESULT_ERROR;
      memcpy (command, si->data, sizeof (SpaControlCmdStateChange));
      break;

    case SPA_CONTROL_CMD_PORT_STATUS_CHANGE:
      fprintf (stderr, "implement iter of %d\n", si->cmd);
      break;

    case SPA_CONTROL_CMD_NEED_INPUT:
      if (si->size < sizeof (SpaControlCmdNeedInput))
        return SPA_RESULT_ERROR;
      memcpy (command, si->data, sizeof (SpaControlCmdNeedInput));
      break;

    case SPA_CONTROL_CMD_HAVE_OUTPUT:
      if (si->size < sizeof (SpaControlCmdHaveOutput))
        return SPA_RESULT_ERROR;
      memcpy (command, si->data, sizeof (SpaControlCmdHaveOutput));
      break;

    /* S -> C */
    case SPA_CONTROL_CMD_ADD_PORT:
      if (si->size < sizeof (SpaControlCmdAddPort))
        return SPA_RESULT_ERROR;
      memcpy (command, si->data, sizeof (SpaControlCmdAddPort));
      break;

    case SPA_CONTROL_CMD_REMOVE_PORT:
      if (si->size < sizeof (SpaControlCmdRemovePort))
        return SPA_RESULT_ERROR;
      memcpy (command, si->data, sizeof (SpaControlCmdRemovePort));
      break;

    case SPA_CONTROL_CMD_SET_FORMAT:
    {
      iter_parse_set_format (si, command);
      break;
    }

    case SPA_CONTROL_CMD_SET_PROPERTY:
      fprintf (stderr, "implement iter of %d\n", si->cmd);
      break;

    case SPA_CONTROL_CMD_START:
    case SPA_CONTROL_CMD_STOP:
      break;

    /* bidirectional */
    case SPA_CONTROL_CMD_ADD_MEM:
      if (si->size < sizeof (SpaControlCmdAddMem))
        return SPA_RESULT_ERROR;
      memcpy (command, si->data, sizeof (SpaControlCmdAddMem));
      break;

    case SPA_CONTROL_CMD_REMOVE_MEM:
      if (si->size < sizeof (SpaControlCmdRemoveMem))
        return SPA_RESULT_ERROR;
      memcpy (command, si->data, sizeof (SpaControlCmdRemoveMem));
      break;

    case SPA_CONTROL_CMD_ADD_BUFFER:
      if (si->size < sizeof (SpaControlCmdAddBuffer))
        return SPA_RESULT_ERROR;
      memcpy (command, si->data, sizeof (SpaControlCmdAddBuffer));
      break;

    case SPA_CONTROL_CMD_REMOVE_BUFFER:
      if (si->size < sizeof (SpaControlCmdRemoveBuffer))
        return SPA_RESULT_ERROR;
      memcpy (command, si->data, sizeof (SpaControlCmdRemoveBuffer));
      break;

    case SPA_CONTROL_CMD_PROCESS_BUFFER:
      if (si->size < sizeof (SpaControlCmdProcessBuffer))
        return SPA_RESULT_ERROR;
      memcpy (command, si->data, sizeof (SpaControlCmdProcessBuffer));
      break;

    case SPA_CONTROL_CMD_REUSE_BUFFER:
      if (si->size < sizeof (SpaControlCmdReuseBuffer))
        return SPA_RESULT_ERROR;
      memcpy (command, si->data, sizeof (SpaControlCmdReuseBuffer));
      break;

    case SPA_CONTROL_CMD_INVALID:
      return SPA_RESULT_ERROR;
  }
  return res;
}


struct stack_builder {
  size_t          magic;

  SpaStackHeader *sh;
  SpaStackControl control;

  SpaControlCmd   cmd;
  size_t          offset;
};

#define SCSB(b)             ((struct stack_builder *) (b))
#define SCSB_MAGIC          ((size_t) 8103647428u)
#define is_valid_builder(b) (b != NULL && \
                             SCSB(b)->magic == SCSB_MAGIC)


/**
 * spa_control_builder_init_full:
 * @builder: a #SpaControlBuilder
 * @version: a version
 * @data: data to build into or %NULL to allocate
 * @max_data: allocated size of @data
 * @fds: memory for fds
 * @max_fds: maximum number of fds in @fds
 *
 * Initialize a stack allocated @builder and set the @version.
 */
SpaResult
spa_control_builder_init_full (SpaControlBuilder *builder,
                               uint32_t           version,
                               void              *data,
                               size_t             max_data,
                               int               *fds,
                               unsigned int       max_fds)
{
  struct stack_builder *sb = SCSB (builder);
  SpaStackHeader *sh;

  if (builder == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  sb->magic = SCSB_MAGIC;

  if (max_data < sizeof (SpaStackHeader) || data == NULL) {
    sb->control.max_size = sizeof (SpaStackHeader) + 128;
    sb->control.data = malloc (sb->control.max_size);
    sb->control.free_data = sb->control.data;
    fprintf (stderr, "builder %p: alloc control memory %zd -> %zd\n",
        builder, max_data, sb->control.max_size);
  } else {
    sb->control.max_size = max_data;
    sb->control.data = data;
    sb->control.free_data = NULL;
  }
  sb->control.size = sizeof (SpaStackHeader);

  sb->control.fds = fds;
  sb->control.max_fds = max_fds;
  sb->control.n_fds = 0;
  sb->control.free_fds = NULL;

  sh = sb->sh = sb->control.data;
  sh->version = version;
  sh->flags = 0;
  sh->length = 0;

  sb->cmd = 0;
  sb->offset = 0;

  return SPA_RESULT_OK;
}

/**
 * spa_control_builder_clear:
 * @builder: a #SpaControlBuilder
 *
 * Clear the memory used by @builder. This can be used to abort building the
 * control.
 *
 * @builder becomes invalid after this function and can be reused with
 * spa_control_builder_init()
 */
SpaResult
spa_control_builder_clear (SpaControlBuilder *builder)
{
  struct stack_builder *sb = SCSB (builder);

  if (!is_valid_builder (builder))
    return SPA_RESULT_INVALID_ARGUMENTS;

  sb->magic = 0;
  free (sb->control.free_data);
  free (sb->control.free_fds);

  return SPA_RESULT_OK;
}

/**
 * spa_control_builder_end:
 * @builder: a #SpaControlBuilder
 * @control: a #SpaControl
 *
 * Ends the building process and fills @control with the constructed
 * #SpaControl.
 *
 * @builder becomes invalid after this function and can be reused with
 * spa_control_builder_init()
 */
SpaResult
spa_control_builder_end (SpaControlBuilder *builder,
                         SpaControl        *control)
{
  struct stack_builder *sb = SCSB (builder);
  SpaStackControl *sc = SSC (control);

  if (!is_valid_builder (builder) || control == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  sb->magic = 0;
  sb->sh->length = sb->control.size - sizeof (SpaStackHeader);

  sc->magic = SSC_MAGIC;
  sc->data = sb->control.data;
  sc->size = sb->control.size;
  sc->max_size = sb->control.max_size;
  sc->free_data = sb->control.free_data;

  sc->fds = sb->control.fds;
  sc->n_fds = sb->control.n_fds;
  sc->max_fds = sb->control.max_fds;
  sc->free_fds = sb->control.free_fds;

  return SPA_RESULT_OK;
}

/**
 * spa_control_builder_add_fd:
 * @builder: a #SpaControlBuilder
 * @fd: a valid fd
 *
 * Add the file descriptor @fd to @builder.
 *
 * Returns: the index of the file descriptor in @builder.
 */
int
spa_control_builder_add_fd (SpaControlBuilder *builder,
                            int                fd,
                            bool               close)
{
  struct stack_builder *sb = SCSB (builder);
  int index;

  if (!is_valid_builder (builder) || fd < 0)
    return -1;

  if (sb->control.n_fds >= sb->control.max_fds) {
    int new_size = sb->control.max_fds + 8;
    fprintf (stderr, "builder %p: realloc control fds %d -> %d\n",
        builder, sb->control.max_fds, new_size);
    sb->control.max_fds = new_size;
    if (sb->control.free_fds == NULL) {
      sb->control.free_fds = malloc (new_size * sizeof (int));
      memcpy (sb->control.free_fds, sb->control.fds, sb->control.n_fds * sizeof (int));
    } else {
      sb->control.free_fds = realloc (sb->control.free_fds, new_size * sizeof (int));
    }
    sb->control.fds = sb->control.free_fds;
  }
  index = sb->control.n_fds;
  sb->control.fds[index] = close ? fd : -fd;
  sb->control.n_fds++;

  return index;
}
#define MAX(a,b)  ((a) > (b) ? (a) : (b))

static void *
builder_ensure_size (struct stack_builder *sb, size_t size)
{
  if (sb->control.size + size > sb->control.max_size) {
    size_t new_size = sb->control.size + MAX (size, 1024);
    fprintf (stderr, "builder %p: realloc control memory %zd -> %zd\n",
        sb, sb->control.max_size, new_size);
    sb->control.max_size = new_size;
    if (sb->control.free_data == NULL) {
      sb->control.free_data = malloc (new_size);
      memcpy (sb->control.free_data, sb->control.data, sb->control.size);
    } else {
      sb->control.free_data = realloc (sb->control.free_data, new_size);
    }
    sb->sh = sb->control.data = sb->control.free_data;
  }
  return (uint8_t *) sb->control.data + sb->control.size;
}

static void *
builder_add_cmd (struct stack_builder *sb, SpaControlCmd cmd, size_t size)
{
  uint8_t *p;
  unsigned int plen;

  plen = 1;
  while (size >> (7 * plen))
    plen++;

  /* 1 for cmd, plen for size and size for payload */
  p = builder_ensure_size (sb, 1 + plen + size);

  sb->cmd = cmd;
  sb->offset = sb->control.size;
  sb->control.size += 1 + plen + size;

  *p++ = cmd;
  /* write length */
  while (plen) {
    plen--;
    *p++ = ((plen > 0) ? 0x80 : 0) | ((size >> (7 * plen)) & 0x7f);
  }
  return p;
}

static void
builder_add_set_format (struct stack_builder *sb, SpaControlCmdSetFormat *sf)
{
  size_t len, slen;
  unsigned int i, j;
  void *p, *base;
  SpaFormat *tf;
  SpaProps *tp;
  const SpaProps *sp;
  SpaPropInfo *pi, *bpi;
  SpaPropRangeInfo *ri, *bri;

  sp = &sf->format->props;

  /* calculate length */
  /* port_id + format + mask  */
  len = sizeof (uint32_t) + sizeof (SpaFormat) + sizeof (uint32_t);
  for (i = 0; i < sp->n_prop_info; i++) {
    pi = (SpaPropInfo *) &sp->prop_info[i];
    len += sizeof (SpaPropInfo);
    len += pi->name ? strlen (pi->name) + 1 : 0;
    len += pi->description ? strlen (pi->description) + 1 : 0;
    /* for the value and the default value */
    len += pi->maxsize + pi->default_size;
    for (j = 0; j < pi->n_range_values; j++) {
      ri = (SpaPropRangeInfo *)&pi->range_values[j];
      len += sizeof (SpaPropRangeInfo);
      len += ri->name ? strlen (ri->name) + 1 : 0;
      len += ri->description ? strlen (ri->description) + 1 : 0;
      /* the size of the range value */
      len += ri->size;
    }
  }

  base = builder_add_cmd (sb, SPA_CONTROL_CMD_SET_FORMAT, len);
  memcpy (base, &sf->port_id, sizeof (uint32_t));

  tf = SPA_MEMBER (base, sizeof (uint32_t), SpaFormat);
  tf->media_type = sf->format->media_type;
  tf->media_subtype = sf->format->media_subtype;

  tp = SPA_MEMBER (tf, offsetof (SpaFormat, props), SpaProps);
  tp->n_prop_info = sp->n_prop_info;
  tp->prop_info = SPA_INT_TO_PTR (sizeof (SpaFormat) + sizeof (uint32_t));

  /* write propinfo array, adjust offset of mask */
  bpi = pi = (SpaPropInfo *) ((uint8_t *)tp + sizeof (SpaFormat) + sizeof (uint32_t));
  for (i = 0; i < tp->n_prop_info; i++) {
    memcpy (pi, &sp->prop_info[i], sizeof (SpaPropInfo));
    pi->mask_offset = sizeof (SpaFormat);
    pi->priv = NULL;
    pi++;
  }
  bri = ri = (SpaPropRangeInfo *) pi;
  pi = bpi;
  /* write range info arrays, adjust offset to it */
  for (i = 0; i < tp->n_prop_info; i++) {
    pi->range_values = SPA_INT_TO_PTR (SPA_PTRDIFF (ri, tp));
    for (j = 0; j < pi->n_range_values; j++) {
      memcpy (ri, &sp->prop_info[i].range_values[j], sizeof (SpaPropRangeInfo));
      ri++;
    }
    pi++;
  }
  p = ri;
  pi = bpi;
  ri = bri;
  /* strings and default values from props and ranges */
  for (i = 0; i < tp->n_prop_info; i++) {
    if (pi->name) {
      slen = strlen (pi->name) + 1;
      memcpy (p, pi->name, slen);
      pi->name = SPA_INT_TO_PTR (SPA_PTRDIFF (p, tp));
      p += slen;
    } else {
      pi->name = 0;
    }
    if (pi->description) {
      slen = strlen (pi->description) + 1;
      memcpy (p, pi->description, slen);
      pi->description = SPA_INT_TO_PTR (SPA_PTRDIFF (p, tp));
      p += slen;
    } else {
      pi->description = 0;
    }
    if (pi->default_value) {
      memcpy (p, pi->default_value, pi->default_size);
      pi->default_value = SPA_INT_TO_PTR (SPA_PTRDIFF (p, tp));
      p += pi->default_size;
    } else {
      pi->default_value = 0;
    }
    for (j = 0; j < pi->n_range_values; j++) {
      if (ri->name) {
        slen = strlen (ri->name) + 1;
        memcpy (p, ri->name, slen);
        ri->name = SPA_INT_TO_PTR (SPA_PTRDIFF (p, tp));
        p += slen;
      } else {
        ri->name = 0;
      }
      if (ri->description) {
        slen = strlen (ri->description) + 1;
        memcpy (p, ri->description, slen);
        ri->description = SPA_INT_TO_PTR (SPA_PTRDIFF (p, tp));
        p += slen;
      } else {
        ri->description = 0;
      }
      if (ri->size) {
        memcpy (p, ri->value, ri->size);
        ri->value = SPA_INT_TO_PTR (SPA_PTRDIFF (p, tp));
        p += ri->size;
      } else {
        ri->value = 0;
      }
      ri++;
    }
    pi++;
  }
  /* and the actual values */
  pi = bpi;
  for (i = 0; i < tp->n_prop_info; i++) {
    if (pi->offset) {
      memcpy (p, SPA_MEMBER (sp, pi->offset, void), pi->maxsize);
      pi->offset = SPA_PTRDIFF (p, tp);
      p += pi->maxsize;
    } else {
      pi->offset = 0;
    }
    pi++;
  }
}

/**
 * spa_control_builder_add_cmd:
 * @builder: a #SpaControlBuilder
 * @cmd: a #SpaControlCmd
 * @command: a command
 *
 * Add a @cmd to @builder with data from @command.
 *
 * Returns: %TRUE on success.
 */
SpaResult
spa_control_builder_add_cmd (SpaControlBuilder *builder,
                             SpaControlCmd      cmd,
                             void              *command)
{
  struct stack_builder *sb = SCSB (builder);
  void *p;
  SpaResult res = SPA_RESULT_OK;

  if (!is_valid_builder (builder))
    return SPA_RESULT_INVALID_ARGUMENTS;

  switch (cmd) {
    /* C -> S */
    case SPA_CONTROL_CMD_NODE_UPDATE:
    case SPA_CONTROL_CMD_PORT_UPDATE:
      fprintf (stderr, "implement builder of %d\n", cmd);
      break;

    case SPA_CONTROL_CMD_PORT_REMOVED:
      p = builder_add_cmd (sb, cmd, sizeof (SpaControlCmdPortRemoved));
      memcpy (p, command, sizeof (SpaControlCmdPortRemoved));
      break;

    case SPA_CONTROL_CMD_STATE_CHANGE:
      p = builder_add_cmd (sb, cmd, sizeof (SpaControlCmdStateChange));
      memcpy (p, command, sizeof (SpaControlCmdStateChange));
      break;

    case SPA_CONTROL_CMD_PORT_STATUS_CHANGE:
      p = builder_add_cmd (sb, cmd, 0);
      break;

    case SPA_CONTROL_CMD_NEED_INPUT:
      p = builder_add_cmd (sb, cmd, sizeof (SpaControlCmdNeedInput));
      memcpy (p, command, sizeof (SpaControlCmdNeedInput));
      break;

    case SPA_CONTROL_CMD_HAVE_OUTPUT:
      p = builder_add_cmd (sb, cmd, sizeof (SpaControlCmdHaveOutput));
      memcpy (p, command, sizeof (SpaControlCmdHaveOutput));
      break;

    /* S -> C */
    case SPA_CONTROL_CMD_ADD_PORT:
      p = builder_add_cmd (sb, cmd, sizeof (SpaControlCmdAddPort));
      memcpy (p, command, sizeof (SpaControlCmdAddPort));
      break;

    case SPA_CONTROL_CMD_REMOVE_PORT:
      p = builder_add_cmd (sb, cmd, sizeof (SpaControlCmdRemovePort));
      memcpy (p, command, sizeof (SpaControlCmdRemovePort));
      break;

    case SPA_CONTROL_CMD_SET_FORMAT:
      builder_add_set_format (sb, command);
      break;

    case SPA_CONTROL_CMD_SET_PROPERTY:
      fprintf (stderr, "implement builder of %d\n", cmd);
      break;

    case SPA_CONTROL_CMD_START:
    case SPA_CONTROL_CMD_STOP:
      p = builder_add_cmd (sb, cmd, 0);
      break;

    /* bidirectional */
    case SPA_CONTROL_CMD_ADD_MEM:
      p = builder_add_cmd (sb, cmd, sizeof (SpaControlCmdAddMem));
      memcpy (p, command, sizeof (SpaControlCmdAddMem));
      break;

    case SPA_CONTROL_CMD_REMOVE_MEM:
      p = builder_add_cmd (sb, cmd, sizeof (SpaControlCmdRemoveMem));
      memcpy (p, command, sizeof (SpaControlCmdRemoveMem));
      break;

    case SPA_CONTROL_CMD_ADD_BUFFER:
      p = builder_add_cmd (sb, cmd, sizeof (SpaControlCmdAddBuffer));
      memcpy (p, command, sizeof (SpaControlCmdAddBuffer));
      break;

    case SPA_CONTROL_CMD_REMOVE_BUFFER:
      p = builder_add_cmd (sb, cmd, sizeof (SpaControlCmdRemoveBuffer));
      memcpy (p, command, sizeof (SpaControlCmdRemoveBuffer));
      break;

    case SPA_CONTROL_CMD_PROCESS_BUFFER:
      p = builder_add_cmd (sb, cmd, sizeof (SpaControlCmdProcessBuffer));
      memcpy (p, command, sizeof (SpaControlCmdProcessBuffer));
      break;

    case SPA_CONTROL_CMD_REUSE_BUFFER:
      p = builder_add_cmd (sb, cmd, sizeof (SpaControlCmdReuseBuffer));
      memcpy (p, command, sizeof (SpaControlCmdReuseBuffer));
      break;

    default:
    case SPA_CONTROL_CMD_INVALID:
      return SPA_RESULT_INVALID_ARGUMENTS;
  }
  return res;
}


SpaResult
spa_control_read (SpaControl   *control,
                  int           fd,
                  void         *data,
                  size_t        max_data,
                  int          *fds,
                  unsigned int  max_fds)
{
  ssize_t len;
  SpaStackHeader *hdr;
  SpaStackControl *sc = (SpaStackControl *) control;
  size_t need;
  struct cmsghdr *cmsg;
  struct msghdr msg = {0};
  struct iovec iov[1];
  char cmsgbuf[CMSG_SPACE (max_fds * sizeof (int))];

  sc->data = data;
  sc->max_size = max_data;
  sc->size = 0;
  sc->free_data = NULL;
  sc->fds = fds;
  sc->max_fds = max_fds;
  sc->n_fds = 0;
  sc->free_fds = NULL;
  hdr = sc->data;

  /* read header and control messages first */
  iov[0].iov_base = hdr;
  iov[0].iov_len = sizeof (SpaStackHeader);;
  msg.msg_iov = iov;
  msg.msg_iovlen = 1;
  msg.msg_control = cmsgbuf;
  msg.msg_controllen = sizeof (cmsgbuf);
  msg.msg_flags = MSG_CMSG_CLOEXEC;

  while (true) {
    len = recvmsg (fd, &msg, msg.msg_flags);
    if (len < 0) {
      if (errno == EINTR)
        continue;
      else
        goto recv_error;
    }
    break;
  }
  if (len != sizeof (SpaStackHeader))
    return SPA_RESULT_ERROR;

  /* now we know the total length */
  need = sizeof (SpaStackHeader) + hdr->length;

  if (sc->max_size < need) {
    fprintf (stderr, "control: realloc receive memory %zd -> %zd\n", sc->max_size, need);
    sc->max_size = need;
    if (sc->free_data == NULL) {
      sc->free_data = malloc (need);
      memcpy (sc->free_data, sc->data, len);
    } else {
      sc->free_data = realloc (sc->free_data, need);
    }
    hdr = sc->data = sc->free_data;
  }
  sc->size = need;

  if (hdr->length > 0) {
    /* read data */
    while (true) {
      len = recv (fd, (uint8_t *)sc->data + sizeof (SpaStackHeader), hdr->length, 0);
      if (len < 0) {
        if (errno == EINTR)
          continue;
        else
          goto recv_error;
      }
      break;
    }
    if (len != hdr->length)
      return SPA_RESULT_ERROR;
  }

  /* handle control messages */
  for (cmsg = CMSG_FIRSTHDR (&msg); cmsg != NULL; cmsg = CMSG_NXTHDR (&msg, cmsg)) {
    if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS)
      continue;

    sc->n_fds = (cmsg->cmsg_len - ((char *)CMSG_DATA (cmsg) - (char *)cmsg)) / sizeof (int);
    memcpy (sc->fds, CMSG_DATA (cmsg), sc->n_fds * sizeof (int));
  }
  sc->magic = SSC_MAGIC;

  SPA_DEBUG_CONTROL ("control %p: read %zd bytes and %d fds\n", sc, len, sc->n_fds);

  return SPA_RESULT_OK;

  /* ERRORS */
recv_error:
  {
    fprintf (stderr, "could not recvmsg: %s\n", strerror (errno));
    return SPA_RESULT_ERROR;
  }
}


SpaResult
spa_control_write (SpaControl *control,
                   int         fd)
{
  SpaStackControl *sc = (SpaStackControl *) control;
  ssize_t len;
  struct msghdr msg = {0};
  struct iovec iov[1];
  struct cmsghdr *cmsg;
  char cmsgbuf[CMSG_SPACE (sc->n_fds * sizeof (int))];
  int fds_len = sc->n_fds * sizeof (int), *cm, i;

  iov[0].iov_base = sc->data;
  iov[0].iov_len = sc->size;
  msg.msg_iov = iov;
  msg.msg_iovlen = 1;
  if (sc->n_fds > 0) {
    msg.msg_control = cmsgbuf;
    msg.msg_controllen = CMSG_SPACE (fds_len);
    cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN (fds_len);
    cm = (int*)CMSG_DATA (cmsg);
    for (i = 0; i < sc->n_fds; i++)
      cm[i] = sc->fds[i] > 0 ? sc->fds[i] : -sc->fds[i];
    msg.msg_controllen = cmsg->cmsg_len;
  } else {
    msg.msg_control = NULL;
    msg.msg_controllen = 0;
  }

  while (true) {
    len = sendmsg (fd, &msg, 0);
    if (len < 0) {
      if (errno == EINTR)
        continue;
      else
        goto send_error;
    }
    break;
  }
  if (len != (ssize_t) sc->size)
    return SPA_RESULT_ERROR;

  SPA_DEBUG_CONTROL ("control %p: written %zd bytes and %d fds\n", sc, len, sc->n_fds);

  return SPA_RESULT_OK;

  /* ERRORS */
send_error:
  {
    fprintf (stderr, "could not sendmsg: %s\n", strerror (errno));
    return SPA_RESULT_ERROR;
  }
}
