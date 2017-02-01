/* Spa V4l2 support
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

#include <stdio.h>

#include <spa/plugin.h>
#include <spa/node.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>

SpaResult spa_ffmpeg_dec_init (SpaHandle *handle, const SpaDict *info, const SpaSupport *support, unsigned int n_support);
SpaResult spa_ffmpeg_enc_init (SpaHandle *handle, const SpaDict *info, const SpaSupport *support, unsigned int n_support);

static SpaResult
ffmpeg_dec_init (const SpaHandleFactory  *factory,
                 SpaHandle               *handle,
                 const SpaDict           *info,
                 const SpaSupport        *support,
                 unsigned int             n_support)
{
  if (factory == NULL || handle == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  return spa_ffmpeg_dec_init (handle, info, support, n_support);
}

static SpaResult
ffmpeg_enc_init (const SpaHandleFactory  *factory,
                 SpaHandle               *handle,
                 const SpaDict           *info,
                 const SpaSupport        *support,
                 unsigned int             n_support)
{
  if (factory == NULL || handle == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  return spa_ffmpeg_enc_init (handle, info, support, n_support);
}

static const SpaInterfaceInfo ffmpeg_interfaces[] =
{
  { SPA_NODE_URI,
  },
};

static SpaResult
ffmpeg_enum_interface_info (const SpaHandleFactory  *factory,
                            const SpaInterfaceInfo **info,
                            unsigned int             index)
{
  if (factory == NULL || info == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  if (index >= 1)
    return SPA_RESULT_ENUM_END;

  *info = &ffmpeg_interfaces[index];

  return SPA_RESULT_OK;
}

SpaResult
spa_enum_handle_factory (const SpaHandleFactory **factory,
                         unsigned int             index)
{
  static const AVCodec *c = NULL;
  static int ci = 0;
  static SpaHandleFactory f;
  static char name[128];

  av_register_all();

  if (index == 0) {
    c = av_codec_next (NULL);
    ci = 0;
  }
  while (index > ci && c) {
    c = av_codec_next (c);
    ci++;
  }
  if (c == NULL)
    return SPA_RESULT_ENUM_END;

  if (av_codec_is_encoder (c)) {
    snprintf (name, 128, "ffenc_%s", c->name);
    f.init = ffmpeg_enc_init;
  }
  else {
    snprintf (name, 128, "ffdec_%s", c->name);
    f.init = ffmpeg_dec_init;
  }
  f.name = name;
  f.info = NULL;
  f.enum_interface_info = ffmpeg_enum_interface_info;

  *factory = &f;

  return SPA_RESULT_OK;
}
