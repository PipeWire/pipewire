/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2025 Alexandre Malki <alexandre.malki@kebag-logic.com> */
/* SPDX-License-Identifier: MIT */


#ifndef __ES_BUILDER_H__
#define __ES_BUILDER_H__

#include "internal.h"

/**
 * This is a mandatory feature to add the necessary state information
 * to create the right entity model
 **/
void es_builder_add_descriptor(struct server *server, uint16_t type,
		uint16_t index, size_t size, void *ptr_aem);


#endif // __ES_BUILDER_H__
