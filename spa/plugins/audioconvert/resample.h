/* Spa */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef RESAMPLE_H
#define RESAMPLE_H

#include <spa/support/cpu.h>
#include <spa/support/log.h>

#define RESAMPLE_DEFAULT_QUALITY	4

struct resample {
	struct spa_log *log;
#define RESAMPLE_OPTION_PREFILL		(1<<0)
	uint32_t options;
	uint32_t cpu_flags;
	const char *func_name;

	uint32_t channels;
	uint32_t i_rate;
	uint32_t o_rate;
	double rate;
	int quality;

	void (*free)		(struct resample *r);
	void (*update_rate)	(struct resample *r, double rate);
	uint32_t (*in_len)	(struct resample *r, uint32_t out_len);
	uint32_t (*out_len)	(struct resample *r, uint32_t in_len);
	void (*process)		(struct resample *r,
				 const void * SPA_RESTRICT src[], uint32_t *in_len,
				 void * SPA_RESTRICT dst[], uint32_t *out_len);
	void (*reset)		(struct resample *r);
	uint32_t (*delay)	(struct resample *r);
	void *data;
};

#define resample_free(r)		(r)->free(r)
#define resample_update_rate(r,...)	(r)->update_rate(r,__VA_ARGS__)
#define resample_in_len(r,...)		(r)->in_len(r,__VA_ARGS__)
#define resample_out_len(r,...)		(r)->out_len(r,__VA_ARGS__)
#define resample_process(r,...)		(r)->process(r,__VA_ARGS__)
#define resample_reset(r)		(r)->reset(r)
#define resample_delay(r)		(r)->delay(r)

int resample_native_init(struct resample *r);
int resample_peaks_init(struct resample *r);

#endif /* RESAMPLE_H */
