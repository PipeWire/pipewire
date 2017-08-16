/* PipeWire
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

#include "pipewire/log.h"

#define USE_POSIX_SHM
#undef JACK_MONITOR

#define JACK_DEFAULT_SERVER_NAME	"default"
#define JACK_SOCKET_DIR			"/dev/shm"
#define JACK_SHM_DIR			"/dev/shm"
#define JACK_SERVER_NAME_SIZE		256
#define JACK_CLIENT_NAME_SIZE		64
#define JACK_PORT_NAME_SIZE		256
#define JACK_PORT_TYPE_SIZE		32
#define JACK_PROTOCOL_VERSION		8
#define JACK_MESSAGE_SIZE		256

#define PORT_NUM_MAX 4096
#define PORT_NUM_FOR_CLIENT 2048
#define CONNECTION_NUM_FOR_PORT PORT_NUM_FOR_CLIENT

#define REAL_JACK_PORT_NAME_SIZE JACK_CLIENT_NAME_SIZE + JACK_PORT_NAME_SIZE

#define BUFFER_SIZE_MAX 8192

#define CLIENT_NUM 256

#define JACK_ENGINE_ROLLING_COUNT 32
#define JACK_ENGINE_ROLLING_INTERVAL 1024

#define TIME_POINTS 100000
#define FAILURE_TIME_POINTS 10000
#define FAILURE_WINDOW 10
#define MEASURED_CLIENTS 32

#define SYNC_MAX_NAME_SIZE 256

#define JACK_UUID_SIZE 36
#define JACK_UUID_STRING_SIZE (JACK_UUID_SIZE+1)

#define JACK_SESSION_COMMAND_SIZE 256

#define NO_PORT 0xFFFE
#define EMPTY   0xFFFD
#define FREE    0xFFFC

typedef enum {
        JACK_TIMER_SYSTEM_CLOCK,
        JACK_TIMER_HPET,
} jack_timer_type_t;

enum jack_request_type {
	jack_request_RegisterPort = 1,
	jack_request_UnRegisterPort = 2,
	jack_request_ConnectPorts = 3,
	jack_request_DisconnectPorts = 4,
	jack_request_SetTimeBaseClient = 5,
	jack_request_ActivateClient = 6,
	jack_request_DeactivateClient = 7,
	jack_request_DisconnectPort = 8,
	jack_request_SetClientCapabilities = 9,
	jack_request_GetPortConnections = 10,
	jack_request_GetPortNConnections = 11,
	jack_request_ReleaseTimebase = 12,
	jack_request_SetTimebaseCallback = 13,
	jack_request_SetBufferSize = 20,
	jack_request_SetFreeWheel = 21,
	jack_request_ClientCheck = 22,
	jack_request_ClientOpen = 23,
	jack_request_ClientClose = 24,
	jack_request_ConnectNamePorts = 25,
	jack_request_DisconnectNamePorts = 26,
	jack_request_GetInternalClientName = 27,
	jack_request_InternalClientHandle = 28,
	jack_request_InternalClientLoad = 29,
	jack_request_InternalClientUnload = 30,
	jack_request_PortRename = 31,
	jack_request_Notification = 32,
	jack_request_SessionNotify = 33,
	jack_request_SessionReply  = 34,
	jack_request_GetClientByUUID = 35,
	jack_request_ReserveClientName = 36,
	jack_request_GetUUIDByClient = 37,
	jack_request_ClientHasSessionCallback = 38,
	jack_request_ComputeTotalLatencies = 39
};

enum jack_notification_type {
	jack_notify_AddClient = 0,
	jack_notify_RemoveClient = 1,
	jack_notify_ActivateClient = 2,
	jack_notify_XRunCallback = 3,
	jack_notify_GraphOrderCallback = 4,
	jack_notify_BufferSizeCallback = 5,
	jack_notify_SampleRateCallback = 6,
	jack_notify_StartFreewheelCallback = 7,
	jack_notify_StopFreewheelCallback = 8,
	jack_notify_PortRegistrationOnCallback = 9,
	jack_notify_PortRegistrationOffCallback = 10,
	jack_notify_PortConnectCallback = 11,
	jack_notify_PortDisconnectCallback = 12,
	jack_notify_PortRenameCallback = 13,
	jack_notify_RealTimeCallback = 14,
	jack_notify_ShutDownCallback = 15,
	jack_notify_QUIT = 16,
	jack_notify_SessionCallback = 17,
	jack_notify_LatencyCallback = 18,
	jack_notify_max = 64  // To keep some room in JackClientControl fCallback table
};

#define kActivateClient_size (2*sizeof(int))
#define kDeactivateClient_size (sizeof(int))
#define kRegisterPort_size (sizeof(int) + JACK_PORT_NAME_SIZE+1 + JACK_PORT_TYPE_SIZE+1 + 2*sizeof(unsigned int))
#define kClientCheck_size (JACK_CLIENT_NAME_SIZE+1 + 4 * sizeof(int))
#define kClientOpen_size (JACK_CLIENT_NAME_SIZE+1 + 2 * sizeof(int))
#define kClientClose_size (sizeof(int))
#define kConnectNamePorts_size (sizeof(int) + REAL_JACK_PORT_NAME_SIZE+1 + REAL_JACK_PORT_NAME_SIZE+1)
#define kGetUUIDByClient_size (JACK_CLIENT_NAME_SIZE+1)

#define CheckRead(var,size) if(read(client->fd,var,size)!=size) {pw_log_error("read error"); return -1; }
#define CheckWrite(var,size) if(send(client->fd,var,size,MSG_NOSIGNAL)!=size) {pw_log_error("write error"); return -1; }
#define CheckSize(expected) { int __size; CheckRead(&__size, sizeof(int)); if (__size != expected) { pw_log_error("CheckSize error size %d != %d", __size, (int)expected); return -1; } }

#define jack_error	pw_log_error
#define jack_log	pw_log_info
