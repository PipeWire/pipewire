/* Spa HSP/HFP native backend HCI support */
/* SPDX-FileCopyrightText: Copyright © 2022 Pauli Virtanen */
/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <unistd.h>
#include <stdarg.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/uio.h>
#include <sys/socket.h>

#include "defs.h"

#ifndef HAVE_BLUEZ_5_HCI

int spa_bt_adapter_has_msbc(struct spa_bt_adapter *adapter)
{
	if (adapter->msbc_probed)
		return adapter->has_msbc;
	return -EOPNOTSUPP;
}

#else

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>

#include <spa/utils/cleanup.h>

int spa_bt_adapter_has_msbc(struct spa_bt_adapter *adapter)
{
	int hci_id;
	spa_autoclose int sock = -1;
	uint8_t features[8], max_page = 0;
	struct sockaddr_hci a;
	const char *str;

	if (adapter->msbc_probed)
		return adapter->has_msbc;

	str = strrchr(adapter->path, '/');  /* hciXX */
	if (str == NULL || sscanf(str, "/hci%d", &hci_id) != 1 || hci_id < 0)
		return -ENOENT;

	sock = socket(AF_BLUETOOTH, SOCK_RAW | SOCK_CLOEXEC, BTPROTO_HCI);
	if (sock < 0)
		return -errno;

	memset(&a, 0, sizeof(a));
	a.hci_family = AF_BLUETOOTH;
	a.hci_dev = hci_id;
	if (bind(sock, (struct sockaddr *) &a, sizeof(a)) < 0)
		return -errno;

	if (hci_read_local_ext_features(sock, 0, &max_page, features, 1000) < 0)
		return -errno;

	adapter->msbc_probed = true;
	adapter->has_msbc = ((features[2] & LMP_TRSP_SCO) && (features[3] & LMP_ESCO)) ? 1 : 0;
	return adapter->has_msbc;
}

#endif
