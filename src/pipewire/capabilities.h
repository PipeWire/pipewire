/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2025 Red Hat */
/* SPDX-License-Identifier: MIT */

#ifndef PIPEWIRE_CAPABILITIESS_H
#define PIPEWIRE_CAPABILITIESS_H

#include <pipewire/utils.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * \defgroup pw_capabilities Capability Names
 *
 * A collection of capabilities that can be advertised by end points in
 * streams.
 *
 * \addtogroup pw_capabilities
 * \{
 */

/**< Link capable of device ID negotiation. The value is to the version of the
 * API specification. */
#define PW_CAPABILITY_DEVICE_ID_NEGOTIATION	"pipewire.device-id-negotiation"
/**< Link with device ID negotition capability supports negotiating with
 * a specific set of devices. The value of API version 1 consists of a JSON
 * object containing a single key "available-devices" that contain a list of
 * hexadecimal encoded `dev_t` device IDs.
 */
#define PW_CAPABILITY_DEVICE_IDS		"pipewire.device-ids"

#define PW_CAPABILITY_DEVICE_ID		"pipewire.device-id"	/**< Link capable of device Id negotation */

/** \}
 */

#ifdef __cplusplus
}
#endif

#endif /* PIPEWIRE_CAPABILITIES_H */
