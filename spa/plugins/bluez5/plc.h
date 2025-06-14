/* Spa PLC */
/* SPDX-FileCopyrightText: Copyright © 2025 Pauli Virtanen */
/* SPDX-License-Identifier: MIT */

#ifndef SPA_BLUEZ5_PLC_H
#define SPA_BLUEZ5_PLC_H

#include <stdint.h>
#include <stdlib.h>
#include <errno.h>

#ifdef HAVE_SPANDSP
#include <spandsp.h>
#else
typedef struct { char dummy; } plc_state_t;
static inline int plc_rx(plc_state_t *s, int16_t *data, int len) { return -ENOTSUP; }
static inline int plc_fillin(plc_state_t *s, int16_t *data, int len) { return -ENOTSUP; }
static inline plc_state_t *plc_init(plc_state_t *s)
{
	static plc_state_t state;
	return &state;
}
static inline int plc_free(plc_state_t *s) { return 0; }
#endif

#endif
