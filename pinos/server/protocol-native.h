/* Pinos
 * Copyright (C) 2017 Wim Taymans <wim.taymans@gmail.com>
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

#include "pinos/client/pinos.h"

typedef void (*PinosDemarshalFunc) (void *object, void *data, size_t size);

extern const PinosCoreEvent pinos_protocol_native_server_core_event;
extern const PinosRegistryEvent pinos_protocol_native_server_registry_event;
extern const PinosModuleEvent pinos_protocol_native_server_module_event;
extern const PinosNodeEvent pinos_protocol_native_server_node_event;
extern const PinosClientEvent pinos_protocol_native_server_client_event;
extern const PinosClientNodeEvent pinos_protocol_native_server_client_node_events;
extern const PinosLinkEvent pinos_protocol_native_server_link_event;

extern const PinosDemarshalFunc pinos_protocol_native_server_core_demarshal[];
extern const PinosDemarshalFunc pinos_protocol_native_server_registry_demarshal[];
extern const PinosDemarshalFunc pinos_protocol_native_server_client_node_demarshal[];
