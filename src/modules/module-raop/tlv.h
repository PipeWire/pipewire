/* PipeWire */
/* SPDX-License-Identifier: MIT */
/*
 * HomeKit TLV8 encode/decode for AirPlay 2 pairing.
 *
 * Derived from the HomeKit Accessory Protocol (HAP) TLV8
 * format, with the AirPlay 2 pairing item types (AP2_TLVType) and error codes
 * (AP2_TLVError).
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

#define TLV_ERROR_MEMORY -1
#define TLV_ERROR_INSUFFICIENT_SIZE -2

typedef enum {
	AP2_TLVType_Method = 0,			// (integer) Method to use for pairing. See PairMethod
	AP2_TLVType_Identifier = 1,		// (UTF-8) Identifier for authentication
	AP2_TLVType_Salt = 2,			// (bytes) 16+ bytes of random salt
	AP2_TLVType_PublicKey = 3,		// (bytes) Curve25519, SRP public key or signed Ed25519 key
	AP2_TLVType_Proof = 4,			// (bytes) Ed25519 or SRP proof
	AP2_TLVType_EncryptedData = 5,	// (bytes) Encrypted data with auth tag at end
	AP2_TLVType_State = 6,			// (integer) State of the pairing process. 1=M1, 2=M2, etc.
	AP2_TLVType_Error = 7,			// (integer) Error code. Must only be present if error code is
									// not 0. See AP2_TLVError
	AP2_TLVType_RetryDelay = 8,		// (integer) Seconds to delay until retrying a setup code
	AP2_TLVType_Certificate = 9,	// (bytes) X.509 Certificate
	AP2_TLVType_Signature = 10,		// (bytes) Ed25519
	AP2_TLVType_Permissions = 11,	// (integer) Bit value describing permissions of the controller
									// being added.
									// None (0x00): Regular user
									// Bit 1 (0x01): Admin that is able to add and remove
									// pairings against the accessory
	AP2_TLVType_FragmentData = 13, 	// (bytes) Non-last fragment of data. If length is 0,
									// it's an ACK.
	AP2_TLVType_FragmentLast = 14,	// (bytes) Last fragment of data
	AP2_TLVType_Flags = 19,			// (integer) HAP kTLVType_Flags (0x13)
	AP2_TLVType_Separator = 0xff,
} AP2_TLVType;

typedef enum {
	AP2_TLVError_Unknown = 1,			// Generic error to handle unexpected errors
	AP2_TLVError_Authentication = 2,	// Setup code or signature verification failed
	AP2_TLVError_Backoff = 3,			// Client must look at the retry delay TLV item and
										// wait that many seconds before retrying
	AP2_TLVError_MaxPeers = 4,			// Server cannot accept any more pairings
	AP2_TLVError_MaxTries = 5,        	// Server reached its maximum number of
										// authentication attempts
	AP2_TLVError_Unavailable = 6,		// Server pairing method is unavailable
	AP2_TLVError_Busy = 7,				// Server is busy and cannot accept a pairing
										// request at this time
} AP2_TLVError;

typedef struct _tlv {
	struct _tlv *next;
	uint8_t type;
	uint8_t *value;
	size_t size;
} tlv_t;

typedef struct {
	tlv_t *head;
	tlv_t *tail;
} tlv_values_t;

tlv_values_t *tlv_new(void);

void tlv_free(tlv_values_t *values);

int tlv_add_value(tlv_values_t *values, uint8_t type, const uint8_t *value, size_t size);

tlv_t *tlv_get_value(const tlv_values_t *values, uint8_t type);

int tlv_format(const tlv_values_t *values, uint8_t *buffer, size_t *size);

int tlv_parse(const uint8_t *buffer, size_t length, tlv_values_t *values);
