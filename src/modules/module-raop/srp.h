/* PipeWire */
/* SPDX-License-Identifier: MIT */
/*
 * SRP-6a client (user side) for AirPlay 2 / HomeKit pairing.
 *
 * Derived from RFC 5054 / RFC 2945 (client side), with the
 * RFC 5054 PAD() rule. Client-only: the RFC 5054 Appendix A 2048/3072-bit
 * groups, SHA-1 / SHA-512, get_N_len(). API-compatible so the pairing call sites are unchanged.
 */

#pragma once

struct SRPUser;

typedef enum {
	SRP_NG_2048,
	SRP_NG_3072
} SRP_NGType;

typedef enum {
	SRP_SHA1,
	SRP_SHA512
} SRP_HashAlgorithm;

struct SRPUser *srp_user_new(SRP_HashAlgorithm alg, SRP_NGType ng_type, const char *username,
	const unsigned char *bytes_password, int len_password, const char *n_hex, const char *g_hex);

void srp_user_free(struct SRPUser *usr);

int get_N_len(SRP_NGType ng_type);

void srp_user_start_authentication(struct SRPUser *usr, const char **username, const unsigned char **bytes_A, int *len_A);

void srp_user_process_challenge(struct SRPUser *usr, const unsigned char *bytes_s, int len_s,
	const unsigned char *bytes_B, int len_B, const unsigned char **bytes_M, int *len_M);

int srp_user_is_authenticated(struct SRPUser *usr);

const unsigned char *srp_user_get_session_key(struct SRPUser *usr, int *key_length);

void srp_user_verify_session(struct SRPUser *usr, const unsigned char *bytes_HAMK);
