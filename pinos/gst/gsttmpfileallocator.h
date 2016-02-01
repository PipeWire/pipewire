/* GStreamer
 * Copyright (C) 2014 William Manley <will@williammanley.net>
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

#ifndef _GST_TMPFILE_ALLOCATOR_H_
#define _GST_TMPFILE_ALLOCATOR_H_

#include <gst/gst.h>

G_BEGIN_DECLS

#define GST_ALLOCATOR_TMPFILE "tmpfile"


/* Allocator that allocates memory from a file stored on a tmpfs */
GstAllocator* gst_tmpfile_allocator_new (void);

gint           gst_tmpfile_memory_get_fd (GstMemory * mem);
gboolean       gst_is_tmpfile_memory     (GstMemory * mem);

G_END_DECLS
#endif /* _GST_TMPFILE_ALLOCATOR_H_ */
