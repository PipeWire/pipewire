/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2021 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef PULSER_SERVER_QUIRKS_H
#define PULSER_SERVER_QUIRKS_H

#include "client.h"

#define QUIRK_FORCE_S16_FORMAT			(1ull<<0)	/** forces S16 sample format in sink and source
								  * info */
#define QUIRK_REMOVE_CAPTURE_DONT_MOVE		(1ull<<1)	/** removes the capture stream DONT_MOVE flag */
#define QUIRK_BLOCK_SOURCE_VOLUME		(1ull<<2)	/** block volume changes to sources */
#define QUIRK_BLOCK_SINK_VOLUME			(1ull<<3)	/** block volume changes to sinks */

int client_update_quirks(struct client *client);

#endif /* PULSER_SERVER_QUIRKS_H */
