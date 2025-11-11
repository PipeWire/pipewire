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

/**< Link capable of device ID negotiation. The value is either "true" or "false" */
#define PW_CAPABILITY_DEVICE_ID_NEGOTIATION	"pipewire.device-id-negotiation"
/**< Link with device ID negotition capability supports negotiating with
 * provided list of devices. The value consists of a JSON encoded string array
 * of base64 encoded dev_t values. */
#define PW_CAPABILITY_DEVICE_IDS		"pipewire.device-ids"

#define PW_CAPABILITY_DEVICE_ID		"pipewire.device-id"	/**< Link capable of device Id negotation */

/** \}
 */

#ifdef __cplusplus
}
#endif

#endif /* PIPEWIRE_CAPABILITIES_H */
