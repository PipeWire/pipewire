/* PipeWire */
/* SPDX-License-Identifier: MIT */
/*
 * srp.c — SRP-6a client (user side) for AirPlay 2 / HomeKit pairing.
 *
 * Derived from SRP-6a: RFC 5054 (SRP for TLS, incl. the
 * 2048/3072-bit groups in Appendix A and the PAD() left-pad-to-len(N) rule) +
 * RFC 2945. CLIENT (user) side only.
 *
 * The receiver drives HKP transient pair-setup with SHA-512 over the 3072-bit
 * group and the identity "Pair-Setup". This module produces the client public
 * value A, verifies the salt/B challenge, derives the shared key K and the proof
 * M1, and checks the server proof M2. Modular arithmetic and hashing run on
 * OpenSSL (BIGNUM + EVP); the ephemeral secret is drawn with RAND_bytes(). Every
 * value that feeds a hash follows the RFC 5054 PAD() rule: the operands of k and
 * u are left-zero-padded to len(N) before hashing, which Apple receivers
 * require. API-compatible so the
 * pairing call sites are unchanged.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/crypto.h>

#include <pipewire/log.h>

#include "srp.h"

/* Widest digest we emit (SHA-512). SHA-1 uses only the first 20 bytes. */
#define SRP_MAX_DIGEST 64

/* len(N) in bytes for the largest supported group (3072-bit). Bounds the stack
 * buffers used for PAD() and minimal-length hashing. */
#define SRP_MAX_N 384

/* -------------------------------------------------------------------------
 * Message digest: a thin selector over OpenSSL's SHA-1 / SHA-512 so the SRP
 * math can stay hash-agnostic.
 * ------------------------------------------------------------------------- */

typedef struct {
	EVP_MD_CTX *ctx;
} md_ctx;

static const EVP_MD *md_algo(SRP_HashAlgorithm alg)
{
	return alg == SRP_SHA1 ? EVP_sha1() : EVP_sha512();
}

static int digest_bytes(SRP_HashAlgorithm alg)
{
	return alg == SRP_SHA1 ? 20 : 64;
}

static void md_start(md_ctx *m, SRP_HashAlgorithm alg)
{
	m->ctx = EVP_MD_CTX_new();
	EVP_DigestInit_ex(m->ctx, md_algo(alg), NULL);
}

static void md_feed(md_ctx *m, const void *data, size_t len)
{
	EVP_DigestUpdate(m->ctx, data, len);
}

static void md_finish(md_ctx *m, uint8_t *out)
{
	EVP_DigestFinal_ex(m->ctx, out, NULL);
	EVP_MD_CTX_free(m->ctx);
	m->ctx = NULL;
}

/* -------------------------------------------------------------------------
 * BIGNUM <-> byte-string helpers. All SRP values travel big-endian; a bare
 * BIGNUM serialises in its minimal length, matching what peers put on the wire.
 * ------------------------------------------------------------------------- */

/* Minimal big-endian byte length of v (0 for a zero value). */
static size_t bn_bytes(const BIGNUM *v)
{
	int n = BN_num_bytes(v);
	return n > 0 ? (size_t) n : 0;
}

/* Serialise v big-endian, minimal length, into dst; returns bytes written. */
static size_t bn_to_be(const BIGNUM *v, uint8_t *dst)
{
	int n = BN_bn2bin(v, dst);
	return n > 0 ? (size_t) n : 0;
}

/* -------------------------------------------------------------------------
 * Groups: the RFC 5054 Appendix A safe-prime moduli and their generators.
 * Indexed by SRP_NGType; n_len is the fixed on-wire width len(N) used for
 * PAD().
 * ------------------------------------------------------------------------- */

struct srp_group {
	int         n_len;
	const char *n_hex;
	const char *g_hex;
};

static const struct srp_group srp_groups[] = {
	[SRP_NG_2048] = {
		256,
		"AC6BDB41324A9A9BF166DE5E1389582FAF72B6651987EE07FC3192943DB56050A37329CBB4"
		"A099ED8193E0757767A13DD52312AB4B03310DCD7F48A9DA04FD50E8083969EDB767B0CF60"
		"95179A163AB3661A05FBD5FAAAE82918A9962F0B93B855F97993EC975EEAA80D740ADBF4FF"
		"747359D041D5C33EA71D281E446B14773BCA97B43A23FB801676BD207A436C6481F1D2B907"
		"8717461A5B9D32E688F87748544523B524B0D57D5EA77A2775D2ECFA032CFBDBF52FB37861"
		"60279004E57AE6AF874E7303CE53299CCC041C7BC308D82A5698F3A8D0C38271AE35F8E9DB"
		"FBB694B5C803D89F7AE435DE236D525F54759B65E372FCD68EF20FA7111F9E4AFF73",
		"2"
	},
	[SRP_NG_3072] = {
		384,
		"FFFFFFFFFFFFFFFFC90FDAA22168C234C4C6628B80DC1CD129024E088A67CC74020BBEA63B"
		"139B22514A08798E3404DDEF9519B3CD3A431B302B0A6DF25F14374FE1356D6D51C245E485"
		"B576625E7EC6F44C42E9A637ED6B0BFF5CB6F406B7EDEE386BFB5A899FA5AE9F24117C4B1F"
		"E649286651ECE45B3DC2007CB8A163BF0598DA48361C55D39A69163FA8FD24CF5F83655D23"
		"DCA3AD961C62F356208552BB9ED529077096966D670C354E4ABC9804F1746C08CA18217C32"
		"905E462E36CE3BE39E772C180E86039B2783A2EC07A28FB5C55DF06F4C52C9DE2BCBF69558"
		"17183995497CEA956AE515D2261898FA051015728E5A8AAAC42DAD33170D04507A33A85521"
		"ABDF1CBA64ECFB850458DBEF0A8AEA71575D060C7DB3970F85A6E1E4C7ABF5AE8CDB0933D7"
		"1E8C94E04A25619DCEE3D2261AD2EE6BF12FFA06D98A0864D87602733EC86A64521F2B1817"
		"7B200CBBE117577A615D6C770988C0BAD946E208E24FA074E5AB3143DB5BFCE0FD108E4B82"
		"D120A93AD2CAFFFFFFFFFFFFFFFF",
		"5"
	},
};

static int group_is_known(SRP_NGType ng_type)
{
	return ng_type == SRP_NG_2048 || ng_type == SRP_NG_3072;
}

int get_N_len(SRP_NGType ng_type)
{
	if (!group_is_known(ng_type))
		return -1;
	return srp_groups[ng_type].n_len;
}

/* -------------------------------------------------------------------------
 * Client session state (opaque to callers).
 * ------------------------------------------------------------------------- */

struct SRPUser {
	SRP_HashAlgorithm alg;

	BIGNUM *N;      /* group modulus */
	BIGNUM *g;      /* group generator */
	int     n_len;  /* len(N) in bytes — PAD() target */

	BIGNUM *a;      /* ephemeral private exponent */
	BIGNUM *A;      /* client public value g^a mod N */

	uint8_t *pub;      /* A serialised as sent on the wire */
	int      pub_len;

	char    *identity;   /* username I */
	uint8_t *secret;     /* password / PIN bytes */
	int      secret_len;

	int      ready;      /* K/M1/M2 have been derived */
	int      confirmed;  /* server proof M2 matched */

	uint8_t  proof[SRP_MAX_DIGEST];    /* M1: our proof to the receiver */
	uint8_t  expect[SRP_MAX_DIGEST];   /* M2: the receiver proof we expect */
	uint8_t  key[SRP_MAX_DIGEST];      /* K: shared session key */
};

/* -------------------------------------------------------------------------
 * SRP hashing primitives, each following the RFC 5054 padding conventions.
 * ------------------------------------------------------------------------- */

/* H(PAD(x) || PAD(y)) reduced back into a BIGNUM, where PAD() left-zero-pads to
 * `width` bytes (len(N)). Used for the multiplier k and the scrambler u. An
 * operand wider than len(N) — only ever possible for an attacker-supplied B — is
 * rejected with -1 instead of overflowing the buffer. */
static int hash_padded_pair(SRP_HashAlgorithm alg, const BIGNUM *x, const BIGNUM *y,
                            int width, BIGNUM *out)
{
	uint8_t digest[SRP_MAX_DIGEST];
	uint8_t buf[2 * SRP_MAX_N];
	size_t  lx = bn_bytes(x);
	size_t  ly = bn_bytes(y);
	size_t  span = (size_t) width * 2;
	md_ctx  md;

	if (width <= 0 || width > SRP_MAX_N || lx > (size_t) width || ly > (size_t) width)
		return -1;

	/* Zeroed so the leading PAD() bytes are genuinely zero before hashing. */
	memset(buf, 0, span);
	bn_to_be(x, buf + ((size_t) width - lx));
	bn_to_be(y, buf + (span - ly));

	md_start(&md, alg);
	md_feed(&md, buf, span);
	md_finish(&md, digest);

	return BN_bin2bn(digest, digest_bytes(alg), out) ? 0 : -1;
}

/* H(minimal(v)) into a raw digest. Used for H(N), H(g) and the session key
 * K = H(S): the operand is hashed at its minimal big-endian length, matching
 * the AirPlay/HomeKit receiver (S is not PAD()ed here). */
static void hash_bignum(SRP_HashAlgorithm alg, const BIGNUM *v, uint8_t *out)
{
	uint8_t tmp[SRP_MAX_N];
	size_t  n = bn_bytes(v);
	md_ctx  md;

	/* Unreachable for the supported groups (all operands < N <= SRP_MAX_N); fail
	 * closed rather than overflow if a caller ever exceeds it. */
	if (n > sizeof tmp) {
		memset(out, 0, (size_t) digest_bytes(alg));
		return;
	}

	bn_to_be(v, tmp);
	md_start(&md, alg);
	md_feed(&md, tmp, n);
	md_finish(&md, out);
	OPENSSL_cleanse(tmp, sizeof tmp);   /* S is secret when hashing the premaster */
}

/* x = H(salt || H(I || ":" || p))  (RFC 2945 / RFC 5054 §2.6). The salt is
 * hashed exactly as delivered by the server. Returns -1 on an empty salt. */
static int derive_x(SRP_HashAlgorithm alg, const uint8_t *salt, int salt_len,
                    const char *identity, const uint8_t *secret, int secret_len,
                    BIGNUM *out)
{
	uint8_t up[SRP_MAX_DIGEST];   /* H(I ":" p) */
	uint8_t xd[SRP_MAX_DIGEST];   /* H(salt || up) */
	md_ctx  md;
	int     ok;

	if (!salt || salt_len <= 0)
		return -1;

	md_start(&md, alg);
	md_feed(&md, identity, strlen(identity));
	md_feed(&md, ":", 1);
	md_feed(&md, secret, secret_len > 0 ? (size_t) secret_len : 0);
	md_finish(&md, up);

	md_start(&md, alg);
	md_feed(&md, salt, (size_t) salt_len);
	md_feed(&md, up, (size_t) digest_bytes(alg));
	md_finish(&md, xd);

	ok = BN_bin2bn(xd, digest_bytes(alg), out) != NULL;
	OPENSSL_cleanse(up, sizeof up);
	OPENSSL_cleanse(xd, sizeof xd);
	return ok ? 0 : -1;
}

/* M1 = H( (H(N) XOR H(g)) || H(I) || salt || A || B || K )  (RFC 2945). The A
 * and B operands are the exact bytes exchanged on the wire, so both ends prove
 * over identical input. */
static void client_proof(SRP_HashAlgorithm alg, const BIGNUM *N, const BIGNUM *g,
                         const char *identity,
                         const uint8_t *salt, int salt_len,
                         const uint8_t *A, int A_len,
                         const uint8_t *B, int B_len,
                         const uint8_t *K, uint8_t *out)
{
	uint8_t h_n[SRP_MAX_DIGEST];
	uint8_t h_g[SRP_MAX_DIGEST];
	uint8_t h_i[SRP_MAX_DIGEST];
	uint8_t xored[SRP_MAX_DIGEST];
	int     dl = digest_bytes(alg);
	int     i;
	md_ctx  md;

	hash_bignum(alg, N, h_n);
	hash_bignum(alg, g, h_g);
	for (i = 0; i < dl; i++)
		xored[i] = h_n[i] ^ h_g[i];

	md_start(&md, alg);
	md_feed(&md, identity, strlen(identity));
	md_finish(&md, h_i);

	md_start(&md, alg);
	md_feed(&md, xored, (size_t) dl);
	md_feed(&md, h_i, (size_t) dl);
	md_feed(&md, salt, (size_t) salt_len);
	md_feed(&md, A, (size_t) A_len);
	md_feed(&md, B, (size_t) B_len);
	md_feed(&md, K, (size_t) dl);
	md_finish(&md, out);
}

/* M2 = H( A || M1 || K ): the server proof the receiver returns to us. */
static void server_proof(SRP_HashAlgorithm alg, const uint8_t *A, int A_len,
                         const uint8_t *M1, const uint8_t *K, uint8_t *out)
{
	int    dl = digest_bytes(alg);
	md_ctx md;

	md_start(&md, alg);
	md_feed(&md, A, (size_t) A_len);
	md_feed(&md, M1, (size_t) dl);
	md_feed(&md, K, (size_t) dl);
	md_finish(&md, out);
}

/* -------------------------------------------------------------------------
 * Public interface (srp.h).
 * ------------------------------------------------------------------------- */

struct SRPUser *srp_user_new(SRP_HashAlgorithm alg, SRP_NGType ng_type, const char *username,
	const unsigned char *bytes_password, int len_password, const char *n_hex, const char *g_hex)
{
	struct SRPUser *usr;
	const char     *n_str;
	const char     *g_str;
	int             width;
	size_t          id_size;

	if (!username || len_password < 0 || (len_password > 0 && !bytes_password))
		return NULL;

	/* Honour an explicit (N, g) pair when the caller supplies both; otherwise
	 * fall back to the RFC 5054 Appendix A group selected by ng_type. */
	if (n_hex && g_hex) {
		n_str = n_hex;
		g_str = g_hex;
		width = -1;                 /* derive len(N) from the parsed modulus */
	} else if (group_is_known(ng_type)) {
		n_str = srp_groups[ng_type].n_hex;
		g_str = srp_groups[ng_type].g_hex;
		width = srp_groups[ng_type].n_len;
	} else {
		return NULL;
	}

	usr = calloc(1, sizeof *usr);
	if (!usr)
		return NULL;

	usr->alg = alg;
	usr->a = BN_new();
	usr->A = BN_new();
	if (!usr->a || !usr->A)
		goto fail;

	if (BN_hex2bn(&usr->N, n_str) == 0 || BN_is_zero(usr->N))
		goto fail;
	if (BN_hex2bn(&usr->g, g_str) == 0 || BN_is_zero(usr->g))
		goto fail;
	usr->n_len = width >= 0 ? width : BN_num_bytes(usr->N);
	if (usr->n_len <= 0 || usr->n_len > SRP_MAX_N)
		goto fail;

	id_size = strlen(username) + 1;
	usr->identity = malloc(id_size);
	usr->secret   = malloc(len_password > 0 ? (size_t) len_password : 1);
	if (!usr->identity || !usr->secret)
		goto fail;

	memcpy(usr->identity, username, id_size);
	if (len_password > 0)
		memcpy(usr->secret, bytes_password, (size_t) len_password);
	usr->secret_len = len_password;

	return usr;

fail:
	srp_user_free(usr);
	return NULL;
}

void srp_user_free(struct SRPUser *usr)
{
	if (!usr)
		return;

	BN_clear_free(usr->a);   /* ephemeral private exponent: scrub before free */
	BN_free(usr->A);
	BN_free(usr->N);
	BN_free(usr->g);

	free(usr->pub);
	free(usr->identity);
	if (usr->secret) {
		OPENSSL_cleanse(usr->secret, usr->secret_len > 0 ? (size_t) usr->secret_len : 1);
		free(usr->secret);
	}

	/* Scrub derived secrets before releasing the block. */
	OPENSSL_cleanse(usr->key,    sizeof usr->key);
	OPENSSL_cleanse(usr->proof,  sizeof usr->proof);
	OPENSSL_cleanse(usr->expect, sizeof usr->expect);
	free(usr);
}

void srp_user_start_authentication(struct SRPUser *usr, const char **username,
	const unsigned char **bytes_A, int *len_A)
{
	uint8_t abuf[32];
	BN_CTX *ctx;

	if (username) *username = NULL;
	if (bytes_A)  *bytes_A = NULL;
	if (len_A)    *len_A = 0;

	/* Ephemeral secret a: 256 bits (RFC 5054 §3). Fail closed on a short read
	 * rather than proceed with weak randomness. */
	if (RAND_bytes(abuf, sizeof abuf) != 1) {
		pw_log_warn("srp: RAND_bytes() failed; cannot start authentication");
		return;
	}
	if (!BN_bin2bn(abuf, sizeof abuf, usr->a)) {
		OPENSSL_cleanse(abuf, sizeof abuf);
		return;
	}
	OPENSSL_cleanse(abuf, sizeof abuf);

	ctx = BN_CTX_new();
	if (!ctx)
		return;
	BN_mod_exp(usr->A, usr->g, usr->a, usr->N, ctx);   /* A = g^a mod N */
	BN_CTX_free(ctx);

	usr->pub_len = BN_num_bytes(usr->A);
	usr->pub = malloc(usr->pub_len ? (size_t) usr->pub_len : 1);
	if (!usr->pub) {
		usr->pub_len = 0;
		return;
	}
	bn_to_be(usr->A, usr->pub);

	if (username) *username = usr->identity;
	if (bytes_A)  *bytes_A = usr->pub;
	if (len_A)    *len_A = usr->pub_len;
}

void srp_user_process_challenge(struct SRPUser *usr, const unsigned char *bytes_s, int len_s,
	const unsigned char *bytes_B, int len_B, const unsigned char **bytes_M, int *len_M)
{
	BIGNUM *B = NULL, *k = NULL, *u = NULL, *x = NULL;
	BIGNUM *tmp = NULL, *base = NULL, *expo = NULL, *S = NULL;
	BN_CTX *ctx = NULL;
	int     dl = digest_bytes(usr->alg);

	if (bytes_M) *bytes_M = NULL;
	if (len_M)   *len_M = 0;

	/* Server input must be present; never assert on a peer-controlled buffer. */
	if (!bytes_s || len_s <= 0 || !bytes_B || len_B <= 0 || !usr->pub)
		return;

	ctx  = BN_CTX_new();
	B    = BN_bin2bn(bytes_B, len_B, NULL);
	k    = BN_new();
	u    = BN_new();
	x    = BN_new();
	tmp  = BN_new();
	base = BN_new();
	expo = BN_new();
	S    = BN_new();
	if (!ctx || !B || !k || !u || !x || !tmp || !base || !expo || !S)
		goto done;

	/* k = H(PAD(N) || PAD(g)) and u = H(PAD(A) || PAD(B)) — both operands
	 * left-padded to len(N) (RFC 5054 §2.6). A B wider than len(N) is rejected
	 * by hash_padded_pair. */
	if (hash_padded_pair(usr->alg, usr->N, usr->g, usr->n_len, k) < 0)
		goto done;
	if (hash_padded_pair(usr->alg, usr->A, B, usr->n_len, u) < 0)
		goto done;

	if (derive_x(usr->alg, bytes_s, len_s, usr->identity,
	             usr->secret, usr->secret_len, x) < 0)
		goto done;

	/* SRP-6a safety checks (RFC 5054 §2.5.4 / §3): abort if B ≡ 0 (mod N) — not
	 * merely B == 0, which would still admit B = N, 2N, … — or if u == 0. */
	if (!BN_nnmod(tmp, B, usr->N, ctx))
		goto done;
	if (BN_is_zero(tmp) || BN_is_zero(u)) {
		pw_log_warn("srp: server public key failed the SRP-6a safety check "
		            "(B mod N == 0 or u == 0); aborting authentication");
		goto done;
	}

	/* S = (B - k*g^x) ^ (a + u*x) mod N.  The base (B - k*g^x) may be negative;
	 * reduce it into [0, N) first so the result is independent of OpenSSL's
	 * handling of a negative BN_mod_exp base. */
	if (!BN_mod_exp(tmp, usr->g, x, usr->N, ctx))   /* tmp  = g^x mod N   */
		goto done;
	if (!BN_mul(tmp, k, tmp, ctx))                  /* tmp  = k * g^x     */
		goto done;
	if (!BN_sub(base, B, tmp))                      /* base = B - k*g^x   */
		goto done;
	if (!BN_nnmod(base, base, usr->N, ctx))         /* base into [0, N)   */
		goto done;
	if (!BN_mul(expo, u, x, ctx))                   /* expo = u * x       */
		goto done;
	if (!BN_add(expo, usr->a, expo))                /* expo = a + u*x     */
		goto done;
	if (!BN_mod_exp(S, base, expo, usr->N, ctx))
		goto done;

	/* K = H(S) over the minimal-length premaster secret. */
	hash_bignum(usr->alg, S, usr->key);

	client_proof(usr->alg, usr->N, usr->g, usr->identity,
	             bytes_s, len_s, usr->pub, usr->pub_len,
	             bytes_B, len_B, usr->key, usr->proof);
	server_proof(usr->alg, usr->pub, usr->pub_len, usr->proof, usr->key, usr->expect);

	usr->ready = 1;
	if (bytes_M) *bytes_M = usr->proof;
	if (len_M)   *len_M = dl;

done:
	/* Scrub the secret-bearing values (x, the intermediates and S itself) before
	 * releasing them; B, k and u hold only public, on-wire data. */
	BN_clear_free(x);
	BN_clear_free(base);
	BN_clear_free(expo);
	BN_clear_free(tmp);
	BN_clear_free(S);
	BN_free(B);
	BN_free(k);
	BN_free(u);
	BN_CTX_free(ctx);
}

int srp_user_is_authenticated(struct SRPUser *usr)
{
	return usr->confirmed;
}

const unsigned char *srp_user_get_session_key(struct SRPUser *usr, int *key_length)
{
	if (key_length)
		*key_length = digest_bytes(usr->alg);
	return usr->key;
}

void srp_user_verify_session(struct SRPUser *usr, const unsigned char *bytes_HAMK)
{
	/* Refuse to authenticate before a session was actually derived: a degenerate
	 * exchange leaves `ready` clear and `expect` all-zero, so a peer echoing a
	 * zero proof must not pass. The compare is constant-time to avoid leaking the
	 * expected verifier through timing. */
	if (!usr->ready || !bytes_HAMK)
		return;

	if (CRYPTO_memcmp(usr->expect, bytes_HAMK, (size_t) digest_bytes(usr->alg)) == 0)
		usr->confirmed = 1;
}
