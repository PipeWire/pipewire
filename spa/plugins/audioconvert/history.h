#include <spa/utils/defs.h>

struct spa_history {
	uint32_t pos;
	uint32_t fill;
	uint32_t max;
	float *history;
};

static inline void spa_history_init(struct spa_history *h, float *history, uint32_t max)
{
	h->history = history;
	h->max = max;
	h->pos = 0;
	h->fill = 0;
}

static inline void spa_history_push(struct spa_history *h, const float *s, uint32_t n)
{
	uint32_t len = SPA_MIN(n, h->max);

	if (h->max <= 1) {
		if (len > 0)
			h->history[0] = s[n-1];
	} else {
		uint32_t l0 = SPA_MIN(len, h->max - h->pos), l1 = len - l0;
		const float *f = &s[n-len];

	        spa_memcpy(&h->history[h->pos], f, l0 * sizeof(float));
	        if (SPA_UNLIKELY(l1 > 0))
			spa_memcpy(h->history, &f[l0], l1 * sizeof(float));

		h->fill = SPA_MIN(h->fill + len, h->max);
		h->pos = (h->pos + len) % h->max;
	}
}
static inline float *spa_history_rotate(struct spa_history *h, uint32_t *len)
{
	uint32_t f = 0, m = h->pos, l = h->max, n = m;
	if (h->fill >= h->max) {
		while (f != n) {
			float t = h->history[n];
			h->history[n++] = h->history[f];
			h->history[f++] = t;
			if (n == l)
				n = m;
			else if (f == m)
				m = n;
		}
	}
	*len = h->fill;
	return h->history;
}
