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
#include "pinos/client/interfaces.h"

typedef void (*PinosDemarshalFunc) (void *object, void *data, size_t size);

extern const PinosCoreInterface       pinos_protocol_native_client_core_interface;
extern const PinosRegistryInterface   pinos_protocol_native_client_registry_interface;
extern const PinosClientNodeInterface pinos_protocol_native_client_client_node_interface;

extern const PinosDemarshalFunc pinos_protocol_native_client_core_demarshal[];
extern const PinosDemarshalFunc pinos_protocol_native_client_module_demarshal[];
extern const PinosDemarshalFunc pinos_protocol_native_client_node_demarshal[];
extern const PinosDemarshalFunc pinos_protocol_native_client_client_node_demarshal[];
extern const PinosDemarshalFunc pinos_protocol_native_client_client_demarshal[];
extern const PinosDemarshalFunc pinos_protocol_native_client_link_demarshal[];
extern const PinosDemarshalFunc pinos_protocol_native_client_registry_demarshal[];
