/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans <wim.taymans@gmail.com> */
/* SPDX-License-Identifier: MIT */

#include <pipewire/pipewire.h>
#include <pipewire/impl.h>
#include <pipewire/extensions/client-node.h>
#include <pipewire/extensions/protocol-native.h>


int main(int argc, char *argv[])
{
	pw_init(&argc, &argv);
	return 0;
}
