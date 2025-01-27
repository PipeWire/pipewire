/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2019 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/mman.h>

#include <jack/jack.h>
#include <jack/midiport.h>

#include <pipewire-jack-extensions.h>

#define MAX_BUFFERS	64

struct data {
	const char *path;

	jack_client_t *client;
	const char *client_name;
	jack_port_t *out_port;

	int cycle;
	uint64_t position;
	uint64_t next_sample;
	uint64_t period;
};

static int
process (jack_nframes_t nframes, void *arg)
{
        struct data *d = (struct data*)arg;
	void *buf;
	uint32_t event[2];

	buf = jack_port_get_buffer (d->out_port, nframes);
	jack_midi_clear_buffer(buf);

	while (d->position >= d->next_sample && d->position + nframes > d->next_sample) {
		uint64_t pos = d->position - d->next_sample;

		if (d->cycle == 0) {
			/* MIDI 2.0 note on, channel 0, middle C, max velocity, no attribute */
			event[0] = 0x40903c00;
			event[1] = 0xffff0000;
		} else {
			/* MIDI 2.0 note off, channel 0, middle C, max velocity, no attribute */
			event[0] = 0x40803c00;
			event[1] = 0xffff0000;
		}

		d->cycle ^= 1;

		jack_midi_event_write(buf, pos, (const jack_midi_data_t *) event, sizeof(event));

		d->next_sample += d->period;
	}
	d->position += nframes;
	return 0;
}

int main(int argc, char *argv[])
{
	struct data data = { 0, };
	jack_options_t options = JackNullOption;
        jack_status_t status;

	data.client = jack_client_open ("ump-source", options, &status);
        if (data.client == NULL) {
                fprintf (stderr, "jack_client_open() failed, "
                         "status = 0x%2.0x\n", status);
                if (status & JackServerFailed) {
                        fprintf (stderr, "Unable to connect to JACK server\n");
                }
                exit (1);
        }
        if (status & JackServerStarted) {
                fprintf (stderr, "JACK server started\n");
        }
        if (status & JackNameNotUnique) {
                data.client_name = jack_get_client_name(data.client);
                fprintf (stderr, "unique name `%s' assigned\n", data.client_name);
        }

	/* send 2 events per second */
	data.period = jack_get_sample_rate(data.client) / 2;

        jack_set_process_callback (data.client, process, &data);

	/* the UMP port type allows both sending and receiving of UMP
	 * messages, which can contain MIDI 1.0 and MIDI 2.0 messages. */
	data.out_port = jack_port_register (data.client, "output",
                                          JACK_DEFAULT_MIDI_TYPE,
                                          JackPortIsOutput | JackPortIsMIDI2, 0);

       if (data.out_port == NULL) {
                fprintf(stderr, "no more JACK ports available\n");
                exit (1);
        }

        if (jack_activate (data.client)) {
                fprintf (stderr, "cannot activate client");
                exit (1);
        }

	while (1) {
		sleep (1);
	}

        jack_client_close (data.client);

	return 0;
}
