/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2024 Dmitry Sharshakov <d3dx12.xx@gmail.com> */
/* SPDX-FileCopyrightText: Copyright © 2025 Nils Tonnaett <ntonnatt@ccrma.stanford.edu> */
/* SPDX-License-Identifier: MIT */

#include "gptp.h"
#include "module-avb/aecp-aem.h"
#include "pipewire/properties.h"
#include "pipewire/protocol.h"

#include <asm/termbits.h>  /* Definition of constants */
#include <bits/time.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <pipewire/log.h>
#include <spa/utils/cleanup.h>
#include <spa/utils/hook.h>
#include <time.h>


struct gptp {
	struct server *server;

	struct pw_loop *loop;
	struct pw_timer_queue *timer_queue;

	struct spa_hook server_listener;

	struct spa_hook_list listener_list;

	struct spa_list attributes;

	char *ptp_mgmt_socket_path;
	int ptp_fd;
	uint32_t ptp_seq;
	uint8_t clock_id[8];
	uint8_t gm_id[8];
};

static int make_unix_ptp_mgmt_socket(const char *path) {
	struct sockaddr_un addr;

	spa_autoclose int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		pw_log_warn("Failed to create PTP management socket");
		return -1;
	}

	int val = 1;
	if (setsockopt(fd, SOL_SOCKET, SO_PASSCRED, &val, sizeof(val)) < 0) {
		pw_log_warn("Failed to bind PTP management socket");
		return -1;
	}

	spa_zero(addr);
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

	if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
		pw_log_warn("Failed to connect PTP management socket");
		return -1;
	}

	return spa_steal_fd(fd);
}

static bool update_ts_refclk(struct gptp *gptp) {
	if (!gptp->ptp_mgmt_socket_path)
		return false;
	if (gptp->ptp_fd < 0) {
		gptp->ptp_fd = make_unix_ptp_mgmt_socket(gptp->ptp_mgmt_socket_path);
		if (gptp->ptp_fd < 0)
			return false;
	}

	// Read if something is left in the socket
	int avail;
	uint8_t tmp;

	ioctl(gptp->ptp_fd, FIONREAD, &avail);
	pw_log_debug("Flushing stale data: %u bytes", avail);
	while (avail-- && read(gptp->ptp_fd, &tmp, 1));

	struct ptp_management_msg req;
	spa_zero(req);

	req.major_sdo_id_message_type = PTP_MESSAGE_TYPE_MANAGEMENT;
	req.ver = PTP_VERSION_1588_2008_2_1;
	req.message_length_be = htobe16(sizeof(struct ptp_management_msg));
	spa_zero(req.clock_identity);
	req.source_port_id_be = htobe16(getpid());
	req.log_message_interval = 127;
	req.sequence_id_be = htobe16(gptp->ptp_seq++);
	memset(req.target_port_identity, 0xff, 8);
	req.target_port_id_be = htobe16(0xffff);
	req.starting_boundary_hops = 1;
	req.boundary_hops = 1;
	req.action = PTP_MGMT_ACTION_GET;
	req.tlv_type_be = htobe16(PTP_TLV_TYPE_MGMT);
	// sent empty TLV, only sending management_id
	req.management_message_length_be = htobe16(2);
	req.management_id_be = htobe16(PTP_MGMT_ID_PARENT_DATA_SET);

	if (write(gptp->ptp_fd, &req, sizeof(req)) == -1) {
		pw_log_warn("Failed to send PTP management request: %m");
		if (errno != ENOTCONN)
			return false;
		close(gptp->ptp_fd);
		gptp->ptp_fd = make_unix_ptp_mgmt_socket(gptp->ptp_mgmt_socket_path);
		if (gptp->ptp_fd > -1)
			pw_log_info("Reopened PTP management socket");
		return false;
	}

	uint8_t buf[sizeof(struct ptp_management_msg) + sizeof(struct ptp_parent_data_set)];
	if (read(gptp->ptp_fd, &buf, sizeof(buf)) == -1) {
		pw_log_warn("Failed to receive PTP management response: %m");
		return false;
	}

	struct ptp_management_msg res = *(struct ptp_management_msg *)buf;
	struct ptp_parent_data_set parent =
		*(struct ptp_parent_data_set *)(buf + sizeof(struct ptp_management_msg));

	if ((res.ver & 0x0f) != 2) {
		pw_log_warn("PTP major version is %d, expected 2", res.ver);
		return false;
	}

	if ((res.major_sdo_id_message_type & 0x0f) != PTP_MESSAGE_TYPE_MANAGEMENT) {
		pw_log_warn("PTP management returned type %x, expected management", res.major_sdo_id_message_type);
		return false;
	}

	if (res.action != PTP_MGMT_ACTION_RESPONSE) {
		pw_log_warn("PTP management returned action %d, expected response", res.action);
		return false;
	}

	if (be16toh(res.tlv_type_be) != PTP_TLV_TYPE_MGMT) {
		pw_log_warn("PTP management returned tlv type %d, expected management", be16toh(res.tlv_type_be));
		return false;
	}

	if (be16toh(res.management_id_be) != PTP_MGMT_ID_PARENT_DATA_SET) {
		pw_log_warn("PTP management returned ID %d, expected PARENT_DATA_SET", be16toh(res.management_id_be));
		return false;
	}

	uint16_t data_len = be16toh(res.management_message_length_be) - 2;
	if (data_len != sizeof(struct ptp_parent_data_set))
		pw_log_warn("Unexpected PTP GET PARENT_DATA_SET response length %u, expected %zu", data_len, sizeof(struct ptp_parent_data_set));

	uint8_t *cid = res.clock_identity;
	if (memcmp(cid, gptp->clock_id, 8) != 0)
		pw_log_info(
			"Local clock ID: IEEE1588-2008:%02X-%02X-%02X-%02X-%02X-%02X-%02X-%02X:%d",
			cid[0],
			cid[1],
			cid[2],
			cid[3],
			cid[4],
			cid[5],
			cid[6],
			cid[7],
			0 /* domain */
	  );

	uint8_t *gmid = parent.gm_clock_id;
	bool gmid_changed = false;
	if (memcmp(gmid, gptp->gm_id, 8) != 0) {
		pw_log_info(
			"GM ID: IEEE1588-2008:%02X-%02X-%02X-%02X-%02X-%02X-%02X-%02X:%d",
			gmid[0],
			gmid[1],
			gmid[2],
			gmid[3],
			gmid[4],
			gmid[5],
			gmid[6],
			gmid[7],
			0 /* domain */
	  );
	  gmid_changed = true;
	}

	// When GM is not equal to own clock we are clocked by external master
	pw_log_debug("Synced to GM: %s", (memcmp(cid, gmid, 8) != 0) ? "true" : "false");

	memcpy(gptp->clock_id, cid, 8);
	memcpy(gptp->gm_id, gmid, 8);
	return gmid_changed;
}

static void gptp_periodic(void *data, uint64_t now)
{
	struct gptp *gptp = data;
	update_ts_refclk(gptp);
}

static void gptp_destroy(void *data)
{
	struct gptp *gptp = data;
	spa_hook_remove(&gptp->server_listener);
	free(gptp);
}

static const struct server_events server_events = {
	AVB_VERSION_SERVER_EVENTS,
	.destroy = gptp_destroy,
	.periodic = gptp_periodic,
};

void avb_gptp_destroy(struct avb_gptp *gptp)
{
	gptp_destroy(gptp);
}

struct avb_gptp *avb_gptp_new(struct server *server)
{
	struct impl *impl;
	struct gptp *gptp;
	const char *str;

	gptp = calloc(1, sizeof(*gptp));
	if (gptp == NULL)
		return NULL;

	gptp->server = server;
	gptp->ptp_fd = -1;

	impl = server->impl;

	gptp->loop = pw_context_get_main_loop(impl->context);
	gptp->timer_queue = pw_context_get_timer_queue(impl->context);

	str = pw_properties_get(impl->props, "ptp.management-socket");
	gptp->ptp_mgmt_socket_path = str ? strdup(str) : NULL;

	if(gptp->ptp_mgmt_socket_path)
		gptp->ptp_fd = make_unix_ptp_mgmt_socket(gptp->ptp_mgmt_socket_path);

	spa_list_init(&gptp->attributes);
	spa_hook_list_init(&gptp->listener_list);

	avdecc_server_add_listener(server, &gptp->server_listener, &server_events, gptp);

	return (struct avb_gptp*)gptp;
}

