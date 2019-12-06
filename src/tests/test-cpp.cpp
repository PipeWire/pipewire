/* PipeWire
 * Copyright (C) 2018 Wim Taymans <wim.taymans@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include <pipewire/array.h>
#include <pipewire/client.h>
#include <pipewire/control.h>
#include <pipewire/core.h>
#include <pipewire/data-loop.h>
#include <pipewire/device.h>
#include <pipewire/factory.h>
#include <pipewire/global.h>
#include <pipewire/interfaces.h>
#include <pipewire/introspect.h>
#include <pipewire/link.h>
#include <pipewire/log.h>
#include <pipewire/loop.h>
#include <pipewire/main-loop.h>
#include <pipewire/map.h>
#include <pipewire/mem.h>
#include <pipewire/module.h>
#include <pipewire/node.h>
#include <pipewire/permission.h>
#include <pipewire/pipewire.h>
#include <pipewire/port.h>
#include <pipewire/private.h>
#include <pipewire/properties.h>
#include <pipewire/protocol.h>
#include <pipewire/proxy.h>
#include <pipewire/core-proxy.h>
#include <pipewire/resource.h>
#include <pipewire/stream.h>
#include <pipewire/thread-loop.h>
#include <pipewire/type.h>
#include <pipewire/utils.h>
#include <pipewire/work-queue.h>
#include <extensions/client-node.h>
#include <extensions/protocol-native.h>


int main(int argc, char *argv[])
{
	pw_init(&argc, &argv);
	return 0;
}
