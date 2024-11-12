/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2021 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <stdint.h>
#include <stddef.h>

#include "audio-dsp.h"

struct convolver *convolver_new(struct spa_fga_dsp *dsp, int block, int tail, const float *ir, int irlen);
void convolver_free(struct convolver *conv);

void convolver_reset(struct convolver *conv);
int convolver_run(struct convolver *conv, const float *input, float *output, int length);
