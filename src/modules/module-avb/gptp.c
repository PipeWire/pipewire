/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2024 Dmitry Sharshakov <d3dx12.xx@gmail.com> */
/* SPDX-FileCopyrightText: Copyright © 2025 Nils Tonnaett <ntonnatt@ccrma.stanford.edu> */
/* SPDX-FileCopyrightText: Copyright © 2026 Alexandre Malki <alexandre.malki@kebag-logic.com> */
/* SPDX-License-Identifier: MIT */

/*
 * Companion ptp4l invocation (gPTP profile, UDS management enabled,
 * Announce path trace recorded):
 *
 *     ptp4l -i <iface> -f /etc/linuxptp/gPTP.cfg \
 *           --uds_address=/var/run/ptp4l \
 *           --path_trace_enabled=1 \
 *           -m
 *
 * pipewire-avb.conf must point ptp.management-socket at the same path:
 *
 *     ptp.management-socket = "/var/run/ptp4l"
 *
 * Equivalent pmc(8) probe to confirm ptp4l accepts our management
 * messages (-t 1 forces transportSpecific = 1 / TS_IEEE_8021AS,
 * matching PTP_GPTP_MANAGEMENT_TYPE in this file):
 *
 *     pmc -u -b 0 -t 1 -s /var/run/ptp4l "GET PARENT_DATA_SET"
 *     pmc -u -b 0 -t 1 -s /var/run/ptp4l "GET PATH_TRACE_LIST"
 */

#include "gptp.h"

#include <errno.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>

#include <pipewire/pipewire.h>
#include <spa/utils/cleanup.h>
#include <spa/utils/hook.h>

#include "aecp-aem-descriptors.h"
#include "aecp-aem-state.h"
#include "entity-model-milan-v12.h"

#define server_emit(s,m,v,...) spa_hook_list_call(&s->listener_list, struct server_events, m, v, ##__VA_ARGS__)
#define server_emit_gm_changed(s, n, g)	server_emit(s, gm_changed, 0, n, g)

#define PTP_REQUEST_INTERVAL_NS	(375 * SPA_NSEC_PER_MSEC)
#define PTP_REQUEST_TIMEOUT_NS	PTP_REQUEST_INTERVAL_NS
#define PTP_LOST_TIMEOUT_THRESHOLD	8

struct gptp {
	struct server *server;

	struct spa_hook server_listener;
	struct spa_source *source;

	char *ptp_mgmt_socket_path;
	int ptp_fd;
	uint16_t ptp_seq;
	uint8_t clock_id[8];
	uint8_t gm_id[8];

	bool req_in_flight;
	uint16_t req_sequence_id;
	uint16_t req_management_id;
	uint64_t req_sent_ns;

	uint32_t tick_count;
	bool data_valid;

	uint32_t consecutive_timeouts;

	uint16_t steps_removed;
	int64_t  offset_from_master_scaled_ns;
	bool data_valid_current;

	uint64_t path_trace[PTP_AS_PATH_MAX_ENTRIES];
	uint16_t path_trace_count;
	bool path_trace_valid;
};

static int make_bind_path(char *out, size_t out_size, uint64_t entity_id)
{
	int len = snprintf(out, out_size,
			"/tmp/pipewire-avb-gptp-%016" PRIx64, entity_id);
	if (len < 0 || (size_t)len >= out_size) {
		return -1;
	}
	return 0;
}

static int make_unix_ptp_mgmt_socket(const char *path, uint64_t entity_id)
{
	struct sockaddr_un addr;
	char bind_path[64];
	int val = 1;

	if (make_bind_path(bind_path, sizeof(bind_path), entity_id) < 0) {
		pw_log_warn("Failed to format PTP management bind path");
		return -1;
	}

	spa_autoclose int fd = socket(AF_UNIX,
			SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (fd < 0) {
		pw_log_warn("Failed to create PTP management socket: %m");
		return -1;
	}

	if (setsockopt(fd, SOL_SOCKET, SO_PASSCRED, &val, sizeof(val)) < 0) {
		pw_log_warn("Failed to set SO_PASSCRED on PTP management socket: %m");
		return -1;
	}

	unlink(bind_path);
	spa_zero(addr);
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, bind_path, sizeof(addr.sun_path) - 1);
	if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
		pw_log_warn("Failed to bind PTP management socket to '%s': %m",
				bind_path);
		return -1;
	}
	pw_log_info("PTP management socket bound to '%s'", bind_path);

	spa_zero(addr);
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
	if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
		pw_log_warn("Failed to connect PTP management socket: %m");
		unlink(bind_path);
		return -1;
	}
	pw_log_info("PTP management socket connected to '%s'", path);

	return spa_steal_fd(fd);
}

static void on_ptp_mgmt_data(void *data, int fd, uint32_t mask);

static void gptp_close_socket(struct gptp *gptp)
{
	char bind_path[64];

	if (gptp->source) {
		pw_loop_destroy_source(gptp->server->impl->loop, gptp->source);
		gptp->source = NULL;
	}
	if (gptp->ptp_fd >= 0) {
		close(gptp->ptp_fd);
		gptp->ptp_fd = -1;
		if (make_bind_path(bind_path, sizeof(bind_path),
				gptp->server->entity_id) == 0) {
			unlink(bind_path);
		}
	}
	gptp->req_in_flight = false;
}

static int gptp_open_socket(struct gptp *gptp)
{
	struct impl *impl = gptp->server->impl;
	int fd;

	fd = make_unix_ptp_mgmt_socket(gptp->ptp_mgmt_socket_path,
			gptp->server->entity_id);
	if (fd < 0) {
		return -1;
	}

	gptp->source = pw_loop_add_io(impl->loop, fd, SPA_IO_IN, false,
			on_ptp_mgmt_data, gptp);
	if (gptp->source == NULL) {
		pw_log_warn("Failed to add PTP management IO source: %m");
		close(fd);
		return -1;
	}
	gptp->ptp_fd = fd;
	return 0;
}

static struct avb_aem_desc_avb_interface *get_avb_interface(struct gptp *gptp)
{
	struct descriptor *d = server_find_descriptor(gptp->server,
			AVB_AEM_DESC_AVB_INTERFACE, 0);
	return d ? descriptor_body(d) : NULL;
}

static void update_avb_interface_clock_identity(struct gptp *gptp,
		const uint8_t cid[8])
{
	struct avb_aem_desc_avb_interface *iface = get_avb_interface(gptp);
	if (iface == NULL) {
		return;
	}
	memcpy(&iface->clock_identity, cid, sizeof(iface->clock_identity));
}

static void mark_gptp_info_dirty(struct gptp *gptp)
{
	struct descriptor *d = server_find_descriptor(gptp->server,
			AVB_AEM_DESC_AVB_INTERFACE, 0);
	struct aecp_aem_avb_interface_state *ifs;

	if (d == NULL) {
		return;
	}
	ifs = d->ptr;
	ifs->gptp_info_dirty = true;
}

static void mark_as_path_dirty(struct gptp *gptp)
{
	struct descriptor *d = server_find_descriptor(gptp->server,
			AVB_AEM_DESC_AVB_INTERFACE, 0);
	struct aecp_aem_avb_interface_state *ifs;

	if (d == NULL) {
		return;
	}
	ifs = d->ptr;
	ifs->as_path_dirty = true;
}

static void gptp_invalidate_state(struct gptp *gptp)
{
	bool had_data = gptp->data_valid || gptp->data_valid_current ||
			gptp->path_trace_valid;
	struct avb_aem_desc_avb_interface *iface;

	gptp->data_valid = false;
	gptp->data_valid_current = false;
	gptp->path_trace_valid = false;
	gptp->path_trace_count = 0;
	gptp->steps_removed = 0;
	gptp->offset_from_master_scaled_ns = 0;
	memset(gptp->clock_id, 0, sizeof(gptp->clock_id));
	memset(gptp->gm_id, 0, sizeof(gptp->gm_id));
	memset(gptp->path_trace, 0, sizeof(gptp->path_trace));

	iface = get_avb_interface(gptp);
	if (iface != NULL) {
		iface->clock_identity = htobe64(gptp->server->entity_id);
		iface->priority1 = DSC_AVB_INTERFACE_PRIORITY1;
		iface->clock_class = DSC_AVB_INTERFACE_CLOCK_CLASS;
		iface->clock_accuracy = DSC_AVB_INTERFACE_CLOCK_ACCURACY;
		iface->offset_scaled_log_variance =
				htons(DSC_AVB_INTERFACE_OFFSET_SCALED_LOG_VARIANCE);
		iface->priority2 = DSC_AVB_INTERFACE_PRIORITY2;
		iface->domain_number = DSC_AVB_INTERFACE_DOMAIN_NUMBER;
		iface->log_sync_interval = DSC_AVB_INTERFACE_LOG_SYNC_INTERVAL;
		iface->log_announce_interval = DSC_AVB_INTERFACE_LOG_ANNOUNCE_INTERVAL;
		iface->log_pdelay_interval = DSC_AVB_INTERFACE_PDELAY_INTERVAL;
		iface->port_number = DSC_AVB_INTERFACE_PORT_NUMBER;
	}

	if (had_data) {
		pw_log_info("PTP management lost contact with ptp4l, "
				"clearing cached gPTP state");
		mark_gptp_info_dirty(gptp);
		mark_as_path_dirty(gptp);
	}
}

static void update_avb_interface_default(struct gptp *gptp,
		const struct ptp_default_data_set *dds)
{
	struct avb_aem_desc_avb_interface *iface = get_avb_interface(gptp);
	if (iface == NULL) {
		return;
	}
	memcpy(&iface->clock_identity, dds->clock_identity,
			sizeof(iface->clock_identity));
	iface->priority1 = dds->priority1;
	iface->clock_class = dds->clock_class;
	iface->clock_accuracy = dds->clock_accuracy;
	iface->offset_scaled_log_variance = dds->offset_scaled_log_variance_be;
	iface->priority2 = dds->priority2;
	iface->domain_number = dds->domain_number;
}

static void update_avb_interface_port(struct gptp *gptp,
		const struct ptp_port_data_set *pds)
{
	struct avb_aem_desc_avb_interface *iface = get_avb_interface(gptp);
	if (iface == NULL) {
		return;
	}
	iface->log_sync_interval = pds->log_sync_interval;
	iface->log_announce_interval = pds->log_announce_interval;
	iface->log_pdelay_interval = pds->log_min_pdelay_req_interval;
	iface->port_number = pds->port_number_be;
}

static int send_management_request(struct gptp *gptp, uint16_t management_id,
		uint64_t now_ns)
{
	struct ptp_management_msg req;
	ssize_t ret;
	uint16_t seq;

	spa_zero(req);

	seq = gptp->ptp_seq++;
	req.major_sdo_id_message_type = PTP_GPTP_MANAGEMENT_TYPE;
	req.ver = PTP_VERSION_1588_2008_2_1;
	req.message_length_be = htons(sizeof(struct ptp_management_msg));
	spa_zero(req.clock_identity);
	req.source_port_id_be = htons((uint16_t)gptp->server->entity_id);
	req.log_message_interval = PTP_DEFAULT_LOG_MESSAGE_INTERVAL;
	req.sequence_id_be = htons(seq);
	memset(req.target_port_identity, 0xff, 8);
	req.target_port_id_be = htons(0xffff);
	req.starting_boundary_hops = 1;
	req.boundary_hops = 1;
	req.action = PTP_MGMT_ACTION_GET;
	req.tlv_type_be = htons(PTP_TLV_TYPE_MGMT);
	req.management_message_length_be = htons(2);
	req.management_id_be = htons(management_id);

	ret = write(gptp->ptp_fd, &req, sizeof(req));
	if (ret == -1) {
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			pw_log_debug("PTP management write would block, skipping tick");
			return -EAGAIN;
		}
		pw_log_warn("Failed to send PTP management request: %m");
		return -errno;
	}
	if (ret != (ssize_t)sizeof(req)) {
		pw_log_warn("Incomplete PTP management request: %zd of %zu bytes",
				ret, sizeof(req));
		return -EIO;
	}

	gptp->req_in_flight = true;
	gptp->req_sequence_id = seq;
	gptp->req_management_id = management_id;
	gptp->req_sent_ns = now_ns;
	pw_log_info("PTP management request sent: id=%04x seq=%u",
			management_id, seq);
	return 0;
}

static void handle_parent_data_set(struct gptp *gptp,
		const struct ptp_management_msg *res,
		const uint8_t *payload, size_t payload_len)
{
	const struct ptp_parent_data_set *parent;
	struct timespec ts;
	uint16_t data_len;
	const uint8_t *cid, *gmid;
	bool gmid_changed = false;
	bool cid_changed = false;

	data_len = ntohs(res->management_message_length_be) - 2;
	if (data_len != sizeof(struct ptp_parent_data_set) ||
			payload_len < sizeof(struct ptp_parent_data_set)) {
		pw_log_warn("Unexpected PTP GET PARENT_DATA_SET response length: "
				"tlv=%u payload=%zu expected=%zu",
				data_len, payload_len,
				sizeof(struct ptp_parent_data_set));
		return;
	}

	parent = (const struct ptp_parent_data_set *)payload;

	cid = res->clock_identity;
	if (memcmp(cid, gptp->clock_id, 8) != 0) {
		pw_log_info("Local clock ID: IEEE1588-2008:"
				"%02X-%02X-%02X-%02X-%02X-%02X-%02X-%02X:%d",
				cid[0], cid[1], cid[2], cid[3],
				cid[4], cid[5], cid[6], cid[7],
				0 /* domain */);
		cid_changed = true;
	}

	gmid = parent->gm_clock_id;
	if (memcmp(gmid, gptp->gm_id, 8) != 0) {
		pw_log_info("GM ID: IEEE1588-2008:"
				"%02X-%02X-%02X-%02X-%02X-%02X-%02X-%02X:%d",
				gmid[0], gmid[1], gmid[2], gmid[3],
				gmid[4], gmid[5], gmid[6], gmid[7],
				0 /* domain */);
		gmid_changed = true;
	}

	pw_log_debug("Synced to GM: %s",
			(memcmp(cid, gmid, 8) != 0) ? "true" : "false");

	memcpy(gptp->clock_id, cid, 8);
	memcpy(gptp->gm_id, gmid, 8);
	gptp->data_valid = true;

	if (cid_changed) {
		update_avb_interface_clock_identity(gptp, cid);
	}

	if (cid_changed || gmid_changed) {
		mark_gptp_info_dirty(gptp);
	}

	if (gmid_changed) {
		clock_gettime(CLOCK_REALTIME, &ts);
		server_emit_gm_changed(gptp->server,
				SPA_TIMESPEC_TO_NSEC(&ts), (uint8_t *)gmid);
	}
}

static void handle_default_data_set(struct gptp *gptp,
		const struct ptp_management_msg *res,
		const uint8_t *payload, size_t payload_len)
{
	const struct ptp_default_data_set *dds;
	uint16_t data_len;

	data_len = ntohs(res->management_message_length_be) - 2;
	if (data_len != sizeof(struct ptp_default_data_set) ||
			payload_len < sizeof(struct ptp_default_data_set)) {
		pw_log_warn("Unexpected PTP GET DEFAULT_DATA_SET response length: "
				"tlv=%u payload=%zu expected=%zu",
				data_len, payload_len,
				sizeof(struct ptp_default_data_set));
		return;
	}
	dds = (const struct ptp_default_data_set *)payload;
	update_avb_interface_default(gptp, dds);
	mark_gptp_info_dirty(gptp);
}

static void handle_port_data_set(struct gptp *gptp,
		const struct ptp_management_msg *res,
		const uint8_t *payload, size_t payload_len)
{
	const struct ptp_port_data_set *pds;
	uint16_t data_len;

	data_len = ntohs(res->management_message_length_be) - 2;
	if (data_len != sizeof(struct ptp_port_data_set) ||
			payload_len < sizeof(struct ptp_port_data_set)) {
		pw_log_warn("Unexpected PTP GET PORT_DATA_SET response length: "
				"tlv=%u payload=%zu expected=%zu",
				data_len, payload_len,
				sizeof(struct ptp_port_data_set));
		return;
	}
	pds = (const struct ptp_port_data_set *)payload;
	update_avb_interface_port(gptp, pds);
}

static void handle_current_data_set(struct gptp *gptp,
		const struct ptp_management_msg *res,
		const uint8_t *payload, size_t payload_len)
{
	const struct ptp_current_data_set *cds;
	uint16_t data_len;
	uint16_t steps_removed;
	int64_t offset_from_master;

	data_len = ntohs(res->management_message_length_be) - 2;
	if (data_len != sizeof(struct ptp_current_data_set) ||
			payload_len < sizeof(struct ptp_current_data_set)) {
		pw_log_warn("Unexpected PTP GET CURRENT_DATA_SET response length: "
				"tlv=%u payload=%zu expected=%zu",
				data_len, payload_len,
				sizeof(struct ptp_current_data_set));
		return;
	}

	cds = (const struct ptp_current_data_set *)payload;
	steps_removed = ntohs(cds->steps_removed_be);
	offset_from_master = (int64_t)be64toh((uint64_t)cds->offset_from_master_be);

	if (!gptp->data_valid_current ||
			gptp->steps_removed != steps_removed) {
		pw_log_info("PTP currentDS: steps_removed=%u offset_from_master=%"
				PRId64 " (scaled ns)",
				steps_removed, offset_from_master);
		mark_gptp_info_dirty(gptp);
	}

	gptp->steps_removed = steps_removed;
	gptp->offset_from_master_scaled_ns = offset_from_master;
	gptp->data_valid_current = true;
}

/* IEEE 1588-2008 Section 16.2.1 PATH_TRACE_LIST */
static void handle_path_trace_list(struct gptp *gptp,
		const struct ptp_management_msg *res,
		const uint8_t *payload, size_t payload_len)
{
	uint16_t data_len;
	uint16_t entries;
	uint16_t i;
	uint64_t new_path[PTP_AS_PATH_MAX_ENTRIES];
	bool changed;

	data_len = ntohs(res->management_message_length_be) - 2;
	if ((data_len % 8) != 0 || payload_len < data_len) {
		pw_log_warn("Unexpected PTP GET PATH_TRACE_LIST response length: "
				"tlv=%u payload=%zu", data_len, payload_len);
		return;
	}

	entries = data_len / 8;
	if (entries > PTP_AS_PATH_MAX_ENTRIES) {
		pw_log_warn("PTP PATH_TRACE_LIST has %u entries, capping to %u",
				entries, PTP_AS_PATH_MAX_ENTRIES);
		entries = PTP_AS_PATH_MAX_ENTRIES;
	}

	for (i = 0; i < entries; i++) {
		memcpy(&new_path[i], payload + i * 8, 8);
	}

	changed = !gptp->path_trace_valid ||
			gptp->path_trace_count != entries ||
			memcmp(gptp->path_trace, new_path,
					entries * sizeof(uint64_t)) != 0;

	if (changed) {
		pw_log_info("PTP path_trace updated: %u entries", entries);
		memcpy(gptp->path_trace, new_path, entries * sizeof(uint64_t));
		gptp->path_trace_count = entries;
		gptp->path_trace_valid = true;
		mark_as_path_dirty(gptp);
	}
}

static void on_ptp_mgmt_data(void *data, int fd, uint32_t mask)
{
	struct gptp *gptp = data;
	uint8_t buf[sizeof(struct ptp_management_msg) + 256];
	struct ptp_management_msg res;
	ssize_t ret;
	uint16_t seq;
	uint16_t mgmt_id;

	if (!(mask & SPA_IO_IN)) {
		return;
	}

	pw_log_info("PTP management socket has data (mask=%#x)", mask);

	for (;;) {
		ret = recv(fd, buf, sizeof(buf), 0);
		if (ret == -1) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				return;
			}
			pw_log_warn("Failed to receive PTP management response: %m");
			return;
		}
		pw_log_info("PTP management received %zd bytes", ret);
		if (ret < (ssize_t)sizeof(struct ptp_management_msg)) {
			pw_log_warn("Received undersized PTP management response: %zd bytes",
					ret);
			continue;
		}

		memcpy(&res, buf, sizeof(res));

		if ((res.ver & 0x0f) != 2) {
			pw_log_warn("PTP major version is %d, expected 2", res.ver);
			continue;
		}

		if ((res.major_sdo_id_message_type & 0x0f) != PTP_MESSAGE_TYPE_MANAGEMENT) {
			pw_log_warn("PTP management returned type %x, expected management",
					res.major_sdo_id_message_type);
			continue;
		}

		if (res.action != PTP_MGMT_ACTION_RESPONSE) {
			pw_log_debug("PTP management returned action %d, expected response",
					res.action);
			continue;
		}

		seq = ntohs(res.sequence_id_be);
		mgmt_id = ntohs(res.management_id_be);

		if (!gptp->req_in_flight || seq != gptp->req_sequence_id) {
			pw_log_debug("Ignoring unsolicited PTP response (seq=%u, id=%04x)",
					seq, mgmt_id);
			continue;
		}

		if (ntohs(res.tlv_type_be) != PTP_TLV_TYPE_MGMT) {
			pw_log_warn("PTP management returned tlv type %d, expected management",
					ntohs(res.tlv_type_be));
			gptp->req_in_flight = false;
			continue;
		}

		if (mgmt_id != gptp->req_management_id) {
			pw_log_warn("PTP management returned ID %04x, expected %04x",
					mgmt_id, gptp->req_management_id);
			gptp->req_in_flight = false;
			continue;
		}

		gptp->consecutive_timeouts = 0;

		switch (mgmt_id) {
		case PTP_MGMT_ID_PARENT_DATA_SET:
			handle_parent_data_set(gptp, &res,
					buf + sizeof(struct ptp_management_msg),
					(size_t)ret - sizeof(struct ptp_management_msg));
			break;
		case PTP_MGMT_ID_DEFAULT_DATA_SET:
			handle_default_data_set(gptp, &res,
					buf + sizeof(struct ptp_management_msg),
					(size_t)ret - sizeof(struct ptp_management_msg));
			break;
		case PTP_MGMT_ID_PORT_DATA_SET:
			handle_port_data_set(gptp, &res,
					buf + sizeof(struct ptp_management_msg),
					(size_t)ret - sizeof(struct ptp_management_msg));
			break;
		case PTP_MGMT_ID_CURRENT_DATA_SET:
			handle_current_data_set(gptp, &res,
					buf + sizeof(struct ptp_management_msg),
					(size_t)ret - sizeof(struct ptp_management_msg));
			break;
		case PTP_MGMT_ID_PATH_TRACE_LIST:
			handle_path_trace_list(gptp, &res,
					buf + sizeof(struct ptp_management_msg),
					(size_t)ret - sizeof(struct ptp_management_msg));
			break;
		default:
			pw_log_debug("Unhandled PTP management ID: %04x", mgmt_id);
			break;
		}
		gptp->req_in_flight = false;
	}
}

static uint16_t next_management_id(uint32_t tick_count)
{
	switch (tick_count % 5) {
	case 0:  return PTP_MGMT_ID_PARENT_DATA_SET;
	case 1:  return PTP_MGMT_ID_PATH_TRACE_LIST;
	case 2:  return PTP_MGMT_ID_CURRENT_DATA_SET;
	case 3:  return PTP_MGMT_ID_DEFAULT_DATA_SET;
	default: return PTP_MGMT_ID_PORT_DATA_SET;
	}
}

static void gptp_periodic(void *data, uint64_t now)
{
	struct gptp *gptp = data;
	int err;

	if (!gptp->ptp_mgmt_socket_path) {
		return;
	}

	if (gptp->ptp_fd < 0 && gptp_open_socket(gptp) < 0) {
		return;
	}

	if (gptp->req_in_flight && (now - gptp->req_sent_ns) > PTP_REQUEST_TIMEOUT_NS) {
		pw_log_debug("PTP management request seq=%u timed out",
				gptp->req_sequence_id);
		gptp->req_in_flight = false;
		gptp->consecutive_timeouts++;
		if (gptp->consecutive_timeouts >= PTP_LOST_TIMEOUT_THRESHOLD) {
			gptp_invalidate_state(gptp);
		}
	}

	if (gptp->req_in_flight) {
		return;
	}

	if (gptp->req_sent_ns != 0 &&
			(now - gptp->req_sent_ns) < PTP_REQUEST_INTERVAL_NS) {
		return;
	}

	err = send_management_request(gptp, next_management_id(gptp->tick_count), now);
	if (err == 0) {
		gptp->tick_count++;
	} else if (err == -ENOTCONN) {
		pw_log_info("PTP management socket disconnected, will reopen");
		gptp_invalidate_state(gptp);
		gptp_close_socket(gptp);
	}
}

static void gptp_destroy(void *data)
{
	struct gptp *gptp = data;
	spa_hook_remove(&gptp->server_listener);

	gptp_close_socket(gptp);

	free(gptp->ptp_mgmt_socket_path);
	free(gptp);
}

static const struct server_events server_events = {
	AVB_VERSION_SERVER_EVENTS,
	.destroy = gptp_destroy,
	.periodic = gptp_periodic,
};

struct avb_gptp *avb_gptp_new(struct server *server)
{
	struct impl *impl;
	struct gptp *gptp;
	const char *str;

	gptp = calloc(1, sizeof(*gptp));
	if (gptp == NULL) {
		return NULL;
	}

	gptp->server = server;
	gptp->ptp_fd = -1;

	impl = server->impl;

	str = pw_properties_get(impl->props, "ptp.management-socket");
	gptp->ptp_mgmt_socket_path = str ? strdup(str) : NULL;

	if (gptp->ptp_mgmt_socket_path) {
		if (gptp_open_socket(gptp) < 0) {
			pw_log_warn("server %p: PTP management socket unavailable, "
					"continuing without GM tracking; will retry on '%s'",
					impl, gptp->ptp_mgmt_socket_path);
		}
	} else {
		pw_log_warn("server %p: ptp.management-socket not set, "
				"continuing without GM tracking", impl);
	}

	avdecc_server_add_listener(server, &gptp->server_listener,
			&server_events, gptp);

	return (struct avb_gptp*)gptp;
}

bool avb_gptp_get_clock_id(const struct avb_gptp *agptp, uint64_t *clock_id_be)
{
	const struct gptp *gptp = (const struct gptp *)agptp;
	if (gptp == NULL || !gptp->data_valid) {
		return false;
	}
	memcpy(clock_id_be, gptp->clock_id, sizeof(*clock_id_be));
	return true;
}

bool avb_gptp_get_grandmaster_id(const struct avb_gptp *agptp, uint64_t *gm_id_be)
{
	const struct gptp *gptp = (const struct gptp *)agptp;
	if (gptp == NULL || !gptp->data_valid) {
		return false;
	}
	memcpy(gm_id_be, gptp->gm_id, sizeof(*gm_id_be));
	return true;
}

bool avb_gptp_is_grandmaster(const struct avb_gptp *agptp)
{
	const struct gptp *gptp = (const struct gptp *)agptp;
	if (gptp == NULL) {
		return false;
	}
	if (gptp->data_valid_current) {
		return gptp->steps_removed == 0;
	}
	if (!gptp->data_valid) {
		return false;
	}
	return memcmp(gptp->clock_id, gptp->gm_id, 8) == 0;
}

uint16_t avb_gptp_get_path_trace(const struct avb_gptp *agptp,
		uint64_t *path_be, uint16_t max_entries)
{
	const struct gptp *gptp = (const struct gptp *)agptp;
	uint16_t count;

	if (gptp == NULL || !gptp->path_trace_valid) {
		return 0;
	}
	count = gptp->path_trace_count;
	if (count > max_entries) {
		count = max_entries;
	}
	memcpy(path_be, gptp->path_trace, count * sizeof(uint64_t));
	return count;
}
