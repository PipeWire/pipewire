/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2023 Wim Taymans <wim.taymans@gmail.com> */
/* SPDX-License-Identifier: MIT */

#ifndef PIPEWIRE_NETJACK2_H
#define PIPEWIRE_NETJACK2_H

#ifdef __cplusplus
extern "C" {
#endif

#define JACK_CLIENT_NAME_SIZE	64
#define JACK_SERVER_NAME_SIZE	256

struct nj2_session_params {
	char type[8];					/* packet type ('param') */
#define NJ2_NETWORK_PROTOCOL	8
	uint32_t version;				/* version */
#define NJ2_ID_FOLLOWER_AVAILABLE	0		/* a follower is available */
#define NJ2_ID_FOLLOWER_SETUP		1		/* follower configuration */
#define NJ2_ID_START_DRIVER		2		/* follower is ready, start driver */
#define NJ2_ID_START_FOLLOWER		3		/* driver is ready, activate follower */
#define NJ2_ID_STOP_DRIVER		4		/* driver must stop */
	int32_t packet_id;				/* indicates the packet type */
	char name[JACK_CLIENT_NAME_SIZE];		/* follower's name */
	char driver_name[JACK_SERVER_NAME_SIZE];	/* driver hostname (network) */
	char follower_name[JACK_SERVER_NAME_SIZE];	/* follower hostname (network) */
	uint32_t mtu;					/* connection mtu */
	uint32_t id;					/* follower's ID */
	uint32_t transport_sync;			/* is the transport synced ? */
	int32_t send_audio_channels;			/* number of driver->follower channels */
	int32_t recv_audio_channels;			/* number of follower->driver channels */
	int32_t send_midi_channels;			/* number of driver->follower midi channels */
	int32_t recv_midi_channels;			/* number of follower->driver midi channels */
	uint32_t sample_rate;				/* session sample rate */
	uint32_t period_size;				/* period size */
#define NJ2_ENCODER_FLOAT	0
#define NJ2_ENCODER_INT		1
#define NJ2_ENCODER_CELT	2
#define NJ2_ENCODER_OPUS	3
	uint32_t sample_encoder;			/* samples encoder */
	uint32_t kbps;					/* KB per second for CELT encoder */
	uint32_t follower_sync_mode;			/* is the follower in sync mode ? */
	uint32_t network_latency;			/* network latency */
} __attribute__ ((packed));

static inline void nj2_dump_session_params(struct nj2_session_params *params)
{
	pw_log_info("Type:          '%s'", params->type);
	pw_log_info("Version:       %u", ntohl(params->version));
	pw_log_info("packet ID:     %d", ntohl(params->packet_id));
	pw_log_info("Name:          '%s'", params->name);
	pw_log_info("Driver Name:   '%s'", params->driver_name);
	pw_log_info("Follower Name: '%s'", params->follower_name);
	pw_log_info("MTU:           %u", ntohl(params->mtu));
	pw_log_info("ID:            %u", ntohl(params->id));
	pw_log_info("TransportSync: %u", ntohl(params->transport_sync));
	pw_log_info("Audio Send:    %d", ntohl(params->send_audio_channels));
	pw_log_info("Audio Recv:    %d", ntohl(params->recv_audio_channels));
	pw_log_info("MIDI Send:     %d", ntohl(params->send_midi_channels));
	pw_log_info("MIDI Recv:     %d", ntohl(params->recv_midi_channels));
	pw_log_info("Sample Rate:   %u", ntohl(params->sample_rate));
	pw_log_info("Period Size:   %u", ntohl(params->period_size));
	pw_log_info("Encoder:       %u", ntohl(params->sample_encoder));
	pw_log_info("KBps:          %u", ntohl(params->kbps));
	pw_log_info("Follower Sync: %u", ntohl(params->follower_sync_mode));
	pw_log_info("Latency:       %u", ntohl(params->network_latency));
}

struct nj2_packet_header {
	char type[8];			/* packet type ('headr') */
	uint32_t data_type;		/* 'a' for audio, 'm' for midi and 's' for sync */
	uint32_t data_stream;		/* 's' for send, 'r' for return */
	uint32_t id;			/* unique ID of the follower */
	uint32_t num_packets;		/* number of data packets of the cycle */
	uint32_t packet_size;		/* packet size in bytes */
	uint32_t active_ports;		/* number of active ports */
	uint32_t cycle;			/* process cycle counter */
	uint32_t sub_cycle;		/* midi/audio subcycle counter */
	int32_t frames;			/* process cycle size in frames (can be -1 to indicate entire buffer) */
	uint32_t is_last;		/* is it the last packet of a given cycle ('y' or 'n') */
} __attribute__ ((packed));

#define UDP_HEADER_SIZE 64		/* 40 bytes for IP header in IPV6, 20 in IPV4, 8 for UDP, so take 64 */

#define PACKET_AVAILABLE_SIZE(mtu) (mtu - UDP_HEADER_SIZE - sizeof(struct nj2_packet_header))

static inline void nj2_dump_packet_header(struct nj2_packet_header *header)
{
	pw_log_info("Type:         %s", header->type);
	pw_log_info("Data Type:    %c", ntohl(header->data_type));
	pw_log_info("Data Stream:  %c", ntohl(header->data_stream));
	pw_log_info("ID:           %u", ntohl(header->id));
	pw_log_info("Num Packets:  %u", ntohl(header->num_packets));
	pw_log_info("Packet Size:  %u", ntohl(header->packet_size));
	pw_log_info("Active Ports: %u", ntohl(header->active_ports));
	pw_log_info("Cycle:        %u", ntohl(header->cycle));
	pw_log_info("Sub Cycle:    %u", ntohl(header->sub_cycle));
	pw_log_info("Frames        %d", ntohl(header->frames));
	pw_log_info("Is Last:      %u", ntohl(header->is_last));
}

struct nj2_midi_event {
	uint32_t time;		/**< Sample index at which event is valid */
	uint32_t size;		/**< Number of bytes of data in the event */
	union {
		uint32_t offset;	/**< offset in buffer */
		uint8_t	buffer[4];	/**< Raw inline data */
	};
};

struct nj2_midi_buffer {
#define MIDI_BUFFER_MAGIC 0x900df00d
	uint32_t magic;
	int32_t buffer_size;
	uint32_t nframes;
	int32_t write_pos;
	uint32_t event_count;
	uint32_t lost_events;

	struct nj2_midi_event event[1];
};

#ifdef __cplusplus
}
#endif

#endif /* PIPEWIRE_NETJACK2_H */
