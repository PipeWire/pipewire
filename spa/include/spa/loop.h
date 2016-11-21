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
typedef struct _SpaLoopControl SpaLoopControl;
typedef struct _SpaLoopUtils SpaLoopUtils;

#define SPA_LOOP_URI             "http://spaplug.in/ns/loop"
#define SPA_LOOP_PREFIX          SPA_LOOP_URI "#"
#define SPA_LOOP__MainLoop       SPA_LOOP_PREFIX "MainLoop"
#define SPA_LOOP__DataLoop       SPA_LOOP_PREFIX "DataLoop"
#define SPA_LOOP__Control        SPA_LOOP_PREFIX "Control"
#define SPA_LOOP__Utils          SPA_LOOP_PREFIX "Utils"

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
 * Register sources and work items to an event loop
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

typedef void (*SpaLoopHook)     (SpaLoopControl *ctrl,
                                 void           *data);
/**
 * SpaLoopControl:
 *
 * Control an event loop
 */
struct _SpaLoopControl {
  /* the total size of this structure. This can be used to expand this
   * structure in the future */
  size_t size;

  int          (*get_fd)            (SpaLoopControl *ctrl);

  void         (*set_hooks)         (SpaLoopControl *ctrl,
                                     SpaLoopHook     pre_hook,
                                     SpaLoopHook     post_hook,
                                     void           *data);

  void         (*enter)             (SpaLoopControl *ctrl);
  void         (*leave)             (SpaLoopControl *ctrl);

  SpaResult    (*iterate)           (SpaLoopControl *ctrl,
                                     int             timeout);
};

#define spa_loop_control_get_fd(l)             (l)->get_fd(l)
#define spa_loop_control_set_hooks(l,...)      (l)->set_hook((l),__VA_ARGS__)
#define spa_loop_control_enter(l)              (l)->enter(l)
#define spa_loop_control_iterate(l,...)        (l)->iterate((l),__VA_ARGS__)
#define spa_loop_control_leave(l)              (l)->leave(l)


typedef void (*SpaSourceIOFunc)     (SpaSource *source,
                                     int        fd,
                                     SpaIO      mask,
                                     void      *data);
typedef void (*SpaSourceIdleFunc)   (SpaSource *source,
                                     void      *data);
typedef void (*SpaSourceEventFunc)  (SpaSource *source,
                                     void      *data);
typedef void (*SpaSourceTimerFunc)  (SpaSource *source,
                                     void      *data);
typedef void (*SpaSourceSignalFunc) (SpaSource *source,
                                     int        signal_number,
                                     void      *data);

/**
 * SpaLoopUtils:
 *
 * Create sources for an event loop
 */
struct _SpaLoopUtils {
  /* the total size of this structure. This can be used to expand this
   * structure in the future */
  size_t size;

  SpaSource *  (*add_io)            (SpaLoopUtils       *utils,
                                     int                 fd,
                                     SpaIO               mask,
                                     bool                close,
                                     SpaSourceIOFunc     func,
                                     void               *data);
  SpaResult    (*update_io)         (SpaSource          *source,
                                     SpaIO               mask);

  SpaSource *  (*add_idle)          (SpaLoopUtils       *utils,
                                     SpaSourceIdleFunc   func,
                                     void               *data);
  void         (*enable_idle)       (SpaSource          *source,
                                     bool                enabled);

  SpaSource *  (*add_event)         (SpaLoopUtils       *utils,
                                     SpaSourceEventFunc  func,
                                     void               *data);
  void         (*signal_event)      (SpaSource          *source);

  SpaSource *  (*add_timer)         (SpaLoopUtils       *utils,
                                     SpaSourceTimerFunc  func,
                                     void               *data);
  SpaResult    (*update_timer)      (SpaSource          *source,
                                     struct timespec    *value,
                                     struct timespec    *interval,
                                     bool                absolute);
  SpaSource *  (*add_signal)        (SpaLoopUtils       *utils,
                                     int                 signal_number,
                                     SpaSourceSignalFunc func,
                                     void               *data);

  void         (*destroy_source)    (SpaSource          *source);
};

#define spa_loop_utils_add_io(l,...)             (l)->add_io(l,__VA_ARGS__)
#define spa_loop_utils_update_io(l,...)          (l)->update_io(__VA_ARGS__)
#define spa_loop_utils_add_idle(l,...)           (l)->add_idle(l,__VA_ARGS__)
#define spa_loop_utils_enable_idle(l,...)        (l)->enable_idle(__VA_ARGS__)
#define spa_loop_utils_add_event(l,...)          (l)->add_event(l,__VA_ARGS__)
#define spa_loop_utils_signal_event(l,...)       (l)->signal_event(__VA_ARGS__)
#define spa_loop_utils_add_timer(l,...)          (l)->add_timer(l,__VA_ARGS__)
#define spa_loop_utils_update_timer(l,...)       (l)->update_timer(__VA_ARGS__)
#define spa_loop_utils_add_signal(l,...)         (l)->add_signal(l,__VA_ARGS__)
#define spa_loop_utils_destroy_source(l,...)     (l)->destroy_source(__VA_ARGS__)

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_LOOP_H__ */
