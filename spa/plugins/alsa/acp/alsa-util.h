#ifndef fooalsautilhfoo
#define fooalsautilhfoo

/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering
  Copyright 2006 Pierre Ossman <ossman@cendio.se> for Cendio AB

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2.1 of the License,
  or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with PulseAudio; if not, see <http://www.gnu.org/licenses/>.
***/

#include <stdbool.h>

#include <alsa/asoundlib.h>

#include "compat.h"

#include "alsa-mixer.h"

int pa_alsa_set_hw_params(
        snd_pcm_t *pcm_handle,
        pa_sample_spec *ss,                /* modified at return */
        snd_pcm_uframes_t *period_size,    /* modified at return */
        snd_pcm_uframes_t *buffer_size,    /* modified at return */
        snd_pcm_uframes_t tsched_size,
        bool *use_mmap,                    /* modified at return */
        bool *use_tsched,                  /* modified at return */
        bool require_exact_channel_number);

int pa_alsa_set_sw_params(
        snd_pcm_t *pcm,
        snd_pcm_uframes_t avail_min,
        bool period_event);

#if 0
/* Picks a working mapping from the profile set based on the specified ss/map */
snd_pcm_t *pa_alsa_open_by_device_id_auto(
        const char *dev_id,
        char **dev,                       /* modified at return */
        pa_sample_spec *ss,               /* modified at return */
        pa_channel_map* map,              /* modified at return */
        int mode,
        snd_pcm_uframes_t *period_size,   /* modified at return */
        snd_pcm_uframes_t *buffer_size,   /* modified at return */
        snd_pcm_uframes_t tsched_size,
        bool *use_mmap,                   /* modified at return */
        bool *use_tsched,                 /* modified at return */
        pa_alsa_profile_set *ps,
        pa_alsa_mapping **mapping);       /* modified at return */
#endif

/* Uses the specified mapping */
snd_pcm_t *pa_alsa_open_by_device_id_mapping(
        const char *dev_id,
        char **dev,                       /* modified at return */
        pa_sample_spec *ss,               /* modified at return */
        pa_channel_map* map,              /* modified at return */
        int mode,
        snd_pcm_uframes_t *period_size,   /* modified at return */
        snd_pcm_uframes_t *buffer_size,   /* modified at return */
        snd_pcm_uframes_t tsched_size,
        bool *use_mmap,                   /* modified at return */
        bool *use_tsched,                 /* modified at return */
        pa_alsa_mapping *mapping);

/* Opens the explicit ALSA device */
snd_pcm_t *pa_alsa_open_by_device_string(
        const char *dir,
        char **dev,                       /* modified at return */
        pa_sample_spec *ss,               /* modified at return */
        pa_channel_map* map,              /* modified at return */
        int mode,
        snd_pcm_uframes_t *period_size,   /* modified at return */
        snd_pcm_uframes_t *buffer_size,   /* modified at return */
        snd_pcm_uframes_t tsched_size,
        bool *use_mmap,                   /* modified at return */
        bool *use_tsched,                 /* modified at return */
        bool require_exact_channel_number);

/* Opens the explicit ALSA device with a fallback list */
snd_pcm_t *pa_alsa_open_by_template(
        char **template,
        const char *dev_id,
        char **dev,                       /* modified at return */
        pa_sample_spec *ss,               /* modified at return */
        pa_channel_map* map,              /* modified at return */
        int mode,
        snd_pcm_uframes_t *period_size,   /* modified at return */
        snd_pcm_uframes_t *buffer_size,   /* modified at return */
        snd_pcm_uframes_t tsched_size,
        bool *use_mmap,                   /* modified at return */
        bool *use_tsched,                 /* modified at return */
        bool require_exact_channel_number);

#if 0
void pa_alsa_dump(pa_log_level_t level, snd_pcm_t *pcm);
void pa_alsa_dump_status(snd_pcm_t *pcm);
#endif

void pa_alsa_refcnt_inc(void);
void pa_alsa_refcnt_dec(void);

void pa_alsa_init_proplist_pcm_info(pa_core *c, pa_proplist *p, snd_pcm_info_t *pcm_info);
void pa_alsa_init_proplist_card(pa_core *c, pa_proplist *p, int card);
void pa_alsa_init_proplist_pcm(pa_core *c, pa_proplist *p, snd_pcm_t *pcm);
#if 0
void pa_alsa_init_proplist_ctl(pa_proplist *p, const char *name);
#endif
bool pa_alsa_init_description(pa_proplist *p, pa_card *card);

char *pa_alsa_get_driver_name(int card);
char *pa_alsa_get_driver_name_by_pcm(snd_pcm_t *pcm);

char *pa_alsa_get_reserve_name(const char *device);

unsigned int *pa_alsa_get_supported_rates(snd_pcm_t *pcm, unsigned int fallback_rate);
pa_sample_format_t *pa_alsa_get_supported_formats(snd_pcm_t *pcm, pa_sample_format_t fallback_format);

bool pa_alsa_pcm_is_hw(snd_pcm_t *pcm);
bool pa_alsa_pcm_is_modem(snd_pcm_t *pcm);

const char* pa_alsa_strerror(int errnum);

#if 0
bool pa_alsa_may_tsched(bool want);
#endif

snd_mixer_elem_t *pa_alsa_mixer_find_card(snd_mixer_t *mixer, const char *name, unsigned int device);
snd_mixer_elem_t *pa_alsa_mixer_find_pcm(snd_mixer_t *mixer, const char *name, unsigned int device);

snd_mixer_t *pa_alsa_open_mixer(pa_hashmap *mixers, int alsa_card_index, bool probe);
snd_mixer_t *pa_alsa_open_mixer_by_name(pa_hashmap *mixers, const char *dev, bool probe);
snd_mixer_t *pa_alsa_open_mixer_for_pcm(pa_hashmap *mixers, snd_pcm_t *pcm, bool probe);

void pa_alsa_mixer_free(pa_alsa_mixer *mixer);

typedef struct pa_hdmi_eld pa_hdmi_eld;
struct pa_hdmi_eld {
    char monitor_name[17];
};

int pa_alsa_get_hdmi_eld(snd_hctl_elem_t *elem, pa_hdmi_eld *eld);

#endif
