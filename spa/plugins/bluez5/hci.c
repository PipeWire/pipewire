/* Spa HSP/HFP native backend HCI support
 *
 * Copyright © 2022 Pauli Virtanen
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

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

int spa_bt_adapter_has_msbc(struct spa_bt_adapter *adapter)
{
	int hci_id, res;
	int sock = -1;
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
		goto error;

	memset(&a, 0, sizeof(a));
	a.hci_family = AF_BLUETOOTH;
	a.hci_dev = hci_id;
	if (bind(sock, (struct sockaddr *) &a, sizeof(a)) < 0)
		goto error;

	if (hci_read_local_ext_features(sock, 0, &max_page, features, 1000) < 0)
		goto error;

	close(sock);

	adapter->msbc_probed = true;
	adapter->has_msbc = ((features[2] & LMP_TRSP_SCO) && (features[3] & LMP_ESCO)) ? 1 : 0;
	return adapter->has_msbc;

error:
	res = -errno;
	if (sock >= 0)
		close(sock);
	return res;
}

#endif
