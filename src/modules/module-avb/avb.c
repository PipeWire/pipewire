/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include "avb.h"
#include "internal.h"

#include <spa/support/cpu.h>

struct pw_avb *pw_avb_new(struct pw_context *context,
		struct pw_properties *props, size_t user_data_size)
{
	struct impl *impl;
	const struct spa_support *support;
	uint32_t n_support;
	struct spa_cpu *cpu;
	const char *str;
	int res = 0;

	impl = calloc(1, sizeof(*impl) + user_data_size);
	if (impl == NULL)
		goto error_exit;

	if (props == NULL)
		props = pw_properties_new(NULL, NULL);
	if (props == NULL)
		goto error_free;

	support = pw_context_get_support(context, &n_support);
	cpu = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_CPU);

	pw_context_conf_update_props(context, "avb.properties", props);

	if ((str = pw_properties_get(props, "vm.overrides")) != NULL) {
		if (cpu != NULL && spa_cpu_get_vm_type(cpu) != SPA_CPU_VM_NONE)
			pw_properties_update_string(props, str, strlen(str));
		pw_properties_set(props, "vm.overrides", NULL);
	}

	impl->context = context;
	impl->loop = pw_context_get_main_loop(context);
	impl->props = props;
	impl->core = pw_context_get_object(context, PW_TYPE_INTERFACE_Core);
	if (impl->core == NULL) {
		str = pw_properties_get(props, PW_KEY_REMOTE_NAME);
		impl->core = pw_context_connect(context,
				pw_properties_new(
					PW_KEY_REMOTE_NAME, str,
					NULL),
				0);
		impl->do_disconnect = true;
	}
	if (impl->core == NULL) {
		res = -errno;
		pw_log_error("can't connect: %m");
		goto error_free;
	}

	spa_list_init(&impl->servers);

	avdecc_server_new(impl, &props->dict);

	return (struct pw_avb*)impl;

error_free:
	free(impl);
error_exit:
	pw_properties_free(props);
	if (res < 0)
		errno = -res;
	return NULL;
}

static void impl_free(struct impl *impl)
{
	struct server *s;

	spa_list_consume(s, &impl->servers, link)
		avdecc_server_free(s);
	free(impl);
}

void pw_avb_destroy(struct pw_avb *avb)
{
	struct impl *impl = (struct impl*)avb;
	impl_free(impl);
}
