/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans */
/* SPDX-FileCopyrightText: Copyright © 2025 Alexandre Malki (alexandre.malki@kebag-logic.com) */
/* SPDX-License-Identifier: MIT */

/**
 * \todo This whole code needs to be re-factore,
 * 	configuring the entity using such a "HARDCODED"
 *	header would does not allow an easy way to
 *	adjust parameters.
 *
 *	Especially for the people involved in the project
 *	and do not have the programming skills to modify
 *	this file.
 *
 * \proposition use a YANG model directly derived from this
 * 	or use the YAML for simplicity.
 *
 * 	Having the YANG would allow directly to know the
 * 	capabilites/limits of the protocol
 */
void init_descriptors(struct server *server);
