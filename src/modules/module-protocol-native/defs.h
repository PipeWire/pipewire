/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

int pw_protocol_native_connect_local_socket(struct pw_protocol_client *client,
					    const struct spa_dict *props,
					    void (*done_callback) (void *data, int res),
					    void *data);
int pw_protocol_native_connect_portal_screencast(struct pw_protocol_client *client,
					    const struct spa_dict *props,
					    void (*done_callback) (void *data, int res),
					    void *data);

static inline void *get_first_pod_from_data(void *data, uint32_t maxsize, uint64_t offset)
{
	void *pod;
	if (maxsize <= offset)
		return NULL;

	/* spa_pod_parser_advance() rounds up, so round down here to compensate */
	maxsize = SPA_ROUND_DOWN_N(maxsize - offset, 8);
	if (maxsize < sizeof(struct spa_pod))
		return NULL;

	pod = SPA_PTROFF(data, offset, void);
	if (SPA_POD_BODY_SIZE(pod) > maxsize - sizeof(struct spa_pod))
		return NULL;
	return pod;
}

struct protocol_compat_v2 {
	/* v2 typemap */
	struct pw_map types;
	unsigned int send_types:1;
};
