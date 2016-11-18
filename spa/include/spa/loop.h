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

#ifndef __SPA_LOOP_H__
#define __SPA_LOOP_H__

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _SpaLoop SpaLoop;
typedef struct _SpaSource SpaSource;

#define SPA_LOOP_URI             "http://spaplug.in/ns/loop"
#define SPA_LOOP_PREFIX          SPA_LOOP_URI "#"
#define SPA_LOOP__MainLoop       SPA_LOOP_PREFIX "MainLoop"
#define SPA_LOOP__DataLoop       SPA_LOOP_PREFIX "DataLoop"

#include <spa/defs.h>

typedef void (*SpaSourceFunc) (SpaSource *source);

typedef enum {
  SPA_IO_IN    = (1 << 0),
  SPA_IO_OUT   = (1 << 1),
  SPA_IO_HUP   = (1 << 2),
  SPA_IO_ERR   = (1 << 3),
} SpaIO;

struct _SpaSource {
  SpaLoop       *loop;
  SpaSourceFunc  func;
  void          *data;
  int            fd;
  SpaIO          mask;
  SpaIO          rmask;
  void          *loop_private;
};

typedef SpaResult (*SpaInvokeFunc) (SpaLoop *loop,
                                    bool     async,
                                    uint32_t seq,
                                    size_t   size,
                                    void    *data,
                                    void    *user_data);
/**
 * SpaLoop:
 *
 * Register sources to an event loop
 */
struct _SpaLoop {
  /* the total size of this structure. This can be used to expand this
   * structure in the future */
  size_t size;

  SpaResult   (*add_source)          (SpaLoop   *loop,
                                      SpaSource *source);
  SpaResult   (*update_source)       (SpaSource *source);

  void        (*remove_source)       (SpaSource *source);

  SpaResult   (*invoke)              (SpaLoop       *loop,
                                      SpaInvokeFunc  func,
                                      uint32_t       seq,
                                      size_t         size,
                                      void          *data,
                                      void          *user_data);
};

#define spa_loop_add_source(l,...)          (l)->add_source((l),__VA_ARGS__)
#define spa_loop_update_source(l,...)       (l)->update_source(__VA_ARGS__)
#define spa_loop_remove_source(l,...)       (l)->remove_source(__VA_ARGS__)
#define spa_loop_invoke(l,...)              (l)->invoke((l),__VA_ARGS__)

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_LOOP_H__ */
