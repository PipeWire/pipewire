/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

/*
 * Protocol footer.
 *
 * For passing around general state data that is not associated with
 * messages sent to objects.
 */

enum {
	FOOTER_CORE_OPCODE_GENERATION = 0,
	FOOTER_CORE_OPCODE_LAST
};

enum {
	FOOTER_CLIENT_OPCODE_GENERATION = 0,
	FOOTER_CLIENT_OPCODE_LAST
};

struct footer_core_global_state {
	uint64_t last_recv_generation;
};

struct footer_client_global_state {
};

struct footer_demarshal {
	int (*demarshal)(void *object, struct spa_pod_parser *parser);
};

extern const struct footer_demarshal footer_core_demarshal[FOOTER_CORE_OPCODE_LAST];
extern const struct footer_demarshal footer_client_demarshal[FOOTER_CLIENT_OPCODE_LAST];

void marshal_core_footers(struct footer_core_global_state *state, struct pw_core *core,
		struct spa_pod_builder *builder);
void marshal_client_footers(struct footer_client_global_state *state, struct pw_impl_client *client,
		struct spa_pod_builder *builder);
