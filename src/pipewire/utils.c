/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include "config.h"

#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#ifdef HAVE_SYS_RANDOM_H
#include <sys/random.h>
#endif
#include <string.h>
#include <time.h>

#include <spa/utils/json.h>

#include <pipewire/array.h>
#include <pipewire/log.h>
#include <pipewire/utils.h>
#include <pipewire/private.h>

/** Split a string based on delimiters
 * \param str a string to split
 * \param delimiter delimiter characters to split on
 * \param[out] len the length of the current string
 * \param[in,out] state a state variable
 * \return a string or NULL when the end is reached
 *
 * Repeatedly call this function to split \a str into all substrings
 * delimited by \a delimiter. \a state should be set to NULL on the first
 * invocation and passed to the function until NULL is returned.
 */
SPA_EXPORT
const char *pw_split_walk(const char *str, const char *delimiter, size_t * len, const char **state)
{
	const char *s = *state ? *state : str;

	s += strspn(s, delimiter);
	if (*s == '\0')
		return NULL;

	*len = strcspn(s, delimiter);
	*state = s + *len;

	return s;
}

/** Split a string based on delimiters
 * \param str a string to split
 * \param delimiter delimiter characters to split on
 * \param max_tokens the max number of tokens to split
 * \param[out] n_tokens the number of tokens
 * \return a NULL terminated array of strings that should be
 *	freed with \ref pw_free_strv.
 */
SPA_EXPORT
char **pw_split_strv(const char *str, const char *delimiter, int max_tokens, int *n_tokens)
{
	const char *state = NULL, *s = NULL;
	struct pw_array arr;
	size_t len;
	int n = 0;

	pw_array_init(&arr, 16);

	s = pw_split_walk(str, delimiter, &len, &state);
	while (s && n + 1 < max_tokens) {
		pw_array_add_ptr(&arr, strndup(s, len));
		s = pw_split_walk(str, delimiter, &len, &state);
		n++;
	}
	if (s) {
		pw_array_add_ptr(&arr, strdup(s));
		n++;
	}
	pw_array_add_ptr(&arr, NULL);

	if (n_tokens != NULL)
		*n_tokens = n;

	return arr.data;
}

/** Split a string in-place based on delimiters
 * \param str a string to split
 * \param delimiter delimiter characters to split on
 * \param max_tokens the max number of tokens to split
 * \param[out] tokens an array to hold up to \a max_tokens of strings
 * \return the number of tokens in \a tokens
 *
 * \a str will be modified in-place so that \a tokens will contain zero terminated
 * strings split at \a delimiter characters.
 */
SPA_EXPORT
int pw_split_ip(char *str, const char *delimiter, int max_tokens, char *tokens[])
{
	const char *state = NULL;
	char *s, *t;
	size_t len, l2;
	int n = 0;

	s = (char *)pw_split_walk(str, delimiter, &len, &state);
	while (s && n + 1 < max_tokens) {
		t = (char*)pw_split_walk(str, delimiter, &l2, &state);
		s[len] = '\0';
		tokens[n++] = s;
		s = t;
		len = l2;
	}
	if (s)
		tokens[n++] = s;
	return n;
}

/** Parse an array of strings
 * \param val a string to parse
 * \param len the length of \a val
 * \param max_tokens the max number of tokens to split
 * \param[out] n_tokens the number of tokens, may be NULL
 * \return a NULL terminated array of strings that should be
 *	freed with \ref pw_free_strv.
 *
 * \a val is parsed using relaxed json syntax.
 *
 * \since 0.3.84
 */
SPA_EXPORT
char **pw_strv_parse(const char *val, size_t len, int max_tokens, int *n_tokens)
{
	struct pw_array arr;
	struct spa_json it[2];
	char v[256];
	int n = 0;

	if (val == NULL)
		return NULL;

	pw_array_init(&arr, 16);

	spa_json_init(&it[0], val, len);
        if (spa_json_enter_array(&it[0], &it[1]) <= 0)
                spa_json_init(&it[1], val, len);

	while (spa_json_get_string(&it[1], v, sizeof(v)) > 0 && n + 1 < max_tokens) {
		pw_array_add_ptr(&arr, strdup(v));
		n++;
	}
	pw_array_add_ptr(&arr, NULL);

	if (n_tokens != NULL)
		*n_tokens = n;

	return arr.data;
}

/** Find a string in a NULL terminated array of strings.
 * \param a a strv to check
 * \param b the string to find
 * \return the index in \a a where \a b is found or < 0 if not.
 *
 * \since 0.3.84
 */
SPA_EXPORT
int pw_strv_find(char **a, const char *b)
{
	int i;
	if (a == NULL || b == NULL)
		return -EINVAL;
	for (i = 0; a[i]; i++) {
		if (spa_streq(a[i], b))
			return i;
	}
	return -ENOENT;
}

/** Check if two NULL terminated arrays of strings have a common string.
 * \param a a strv to check
 * \param b another strv to check
 * \return the index in \a a of the first common string or < 0 if not.
 *
 * \since 0.3.84
 */
SPA_EXPORT
int pw_strv_find_common(char **a, char **b)
{
	int i;

	if (a == NULL || b == NULL)
		return -EINVAL;

	for (i = 0; a[i]; i++) {
		if (pw_strv_find(b, a[i]) >= 0)
			return i;
	}
	return -ENOENT;
}

/** Free a NULL terminated array of strings
 * \param str a NULL terminated array of string
 *
 * Free all the strings in the array and the array
 */
SPA_EXPORT
void pw_free_strv(char **str)
{
	int i;

	if (str == NULL)
		return;

	for (i = 0; str[i]; i++)
		free(str[i]);
	free(str);
}

/** Strip all whitespace before and after a string
 * \param str a string to strip
 * \param whitespace characters to strip
 * \return the stripped part of \a str
 *
 * Strip whitespace before and after \a str. \a str will be
 * modified.
 */
SPA_EXPORT
char *pw_strip(char *str, const char *whitespace)
{
	char *e, *l = NULL;

	str += strspn(str, whitespace);

	for (e = str; *e; e++)
		if (!strchr(whitespace, *e))
			l = e;

	if (l)
		*(l + 1) = '\0';
	else
		*str = '\0';

	return str;
}

static inline ssize_t make_random(void *buf, size_t buflen, unsigned int flags)
{
	ssize_t bytes;

#ifdef HAVE_GETRANDOM
	bytes = getrandom(buf, buflen, flags);
	if (bytes < 0)
		bytes = -errno;
	if (bytes != -ENOSYS)
		return bytes;
#endif

	int fd = open("/dev/urandom", O_CLOEXEC);
	if (fd < 0)
		return -errno;

	bytes = read(fd, buf, buflen);
	if (bytes < 0)
		bytes = -errno;

	close(fd);

	return bytes;
}

/** Fill a buffer with random data
 * \param buf a buffer to fill
 * \param buflen the number of bytes to fill
 * \param flags optional flags
 * \return the number of bytes filled
 *
 * Fill \a buf with \a buflen random bytes.
 */
SPA_EXPORT
ssize_t pw_getrandom(void *buf, size_t buflen, unsigned int flags)
{
	ssize_t res;
	do {
		res = make_random(buf, buflen, flags);
	} while (res == -EINTR);
	if (res < 0)
		return res;
	if ((size_t)res != buflen)
		return -ENODATA;
	return res;
}

#ifdef HAVE_RANDOM_R
static char statebuf[256];
static struct random_data random_state;
#endif

/** Fill a buffer with random data
 * \param buf a buffer to fill
 * \param buflen the number of bytes to fill
 *
 * Fill \a buf with \a buflen random bytes. This functions uses
 * pw_getrandom() but falls back to a pseudo random number
 * generator in case of failure.
 */
SPA_EXPORT
void pw_random(void *buf, size_t buflen)
{
	if (pw_getrandom(buf, buflen, 0) < 0) {
		uint8_t *p = buf;
		while (buflen-- > 0) {
			int32_t val;
#ifdef HAVE_RANDOM_R
			random_r(&random_state, &val);
#else
			val = rand();
#endif
			*p++ = (uint8_t) val;;
		}
	}
}

void pw_random_init(void)
{
	unsigned int seed;
	if (pw_getrandom(&seed, sizeof(seed), 0) < 0) {
		struct timespec ts;
		clock_gettime(CLOCK_REALTIME, &ts);
		seed = (unsigned int) SPA_TIMESPEC_TO_NSEC(&ts);
	}
#ifdef HAVE_RANDOM_R
	initstate_r(seed, statebuf, sizeof(statebuf), &random_state);
#else
	srand(seed);
#endif
}

SPA_EXPORT
void* pw_reallocarray(void *ptr, size_t nmemb, size_t size)
{
#ifdef HAVE_REALLOCARRAY
	return reallocarray(ptr, nmemb, size);
#else
	return realloc(ptr, nmemb * size);
#endif
}
