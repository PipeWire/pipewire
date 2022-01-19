/* PipeWire
 *
 * Copyright © 2022 Wim Taymans
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

#include "config.h"

#include <stdio.h>
#include <unistd.h>
#include <errno.h>

#include <jack/net.h>

#include <pipewire/pipewire.h>

SPA_EXPORT
jack_net_slave_t* jack_net_slave_open(const char* ip, int port, const char* name,
		jack_slave_t* request, jack_master_t* result)
{
	return NULL;
}

SPA_EXPORT
int jack_net_slave_close(jack_net_slave_t* net)
{
	return ENOTSUP;
}

SPA_EXPORT
int jack_set_net_slave_process_callback(jack_net_slave_t * net, JackNetSlaveProcessCallback net_callback, void *arg)
{
	return ENOTSUP;
}

SPA_EXPORT
int jack_net_slave_activate(jack_net_slave_t* net)
{
	return ENOTSUP;
}

SPA_EXPORT
int jack_net_slave_deactivate(jack_net_slave_t* net)
{
	return ENOTSUP;
}

SPA_EXPORT
int jack_net_slave_is_active(jack_net_slave_t* net)
{
	return false;
}

SPA_EXPORT
int jack_set_net_slave_buffer_size_callback(jack_net_slave_t *net, JackNetSlaveBufferSizeCallback bufsize_callback, void *arg)
{
	return ENOTSUP;
}

SPA_EXPORT
int jack_set_net_slave_sample_rate_callback(jack_net_slave_t *net, JackNetSlaveSampleRateCallback samplerate_callback, void *arg)
{
	return ENOTSUP;
}

SPA_EXPORT
int jack_set_net_slave_shutdown_callback(jack_net_slave_t *net, JackNetSlaveShutdownCallback shutdown_callback, void *arg)
{
	return ENOTSUP;
}

SPA_EXPORT
int jack_set_net_slave_restart_callback(jack_net_slave_t *net, JackNetSlaveRestartCallback restart_callback, void *arg)
{
	return ENOTSUP;
}

SPA_EXPORT
int jack_set_net_slave_error_callback(jack_net_slave_t *net, JackNetSlaveErrorCallback error_callback, void *arg)
{
	return ENOTSUP;
}

SPA_EXPORT
jack_net_master_t* jack_net_master_open(const char* ip, int port, jack_master_t* request, jack_slave_t* result)
{
	return NULL;
}

SPA_EXPORT
int jack_net_master_close(jack_net_master_t* net)
{
	return ENOTSUP;
}

SPA_EXPORT
int jack_net_master_recv(jack_net_master_t* net, int audio_input, float** audio_input_buffer, int midi_input, void** midi_input_buffer)
{
	return ENOTSUP;
}

SPA_EXPORT
int jack_net_master_recv_slice(jack_net_master_t* net, int audio_input, float** audio_input_buffer, int midi_input, void** midi_input_buffer, int frames)
{
	return ENOTSUP;
}

SPA_EXPORT
int jack_net_master_send(jack_net_master_t* net, int audio_output, float** audio_output_buffer, int midi_output, void** midi_output_buffer)
{
	return ENOTSUP;
}

SPA_EXPORT
int jack_net_master_send_slice(jack_net_master_t* net, int audio_output, float** audio_output_buffer, int midi_output, void** midi_output_buffer, int frames)
{
	return ENOTSUP;
}

SPA_EXPORT
jack_adapter_t* jack_create_adapter(int input, int output,
                                    jack_nframes_t host_buffer_size,
                                    jack_nframes_t host_sample_rate,
                                    jack_nframes_t adapted_buffer_size,
                                    jack_nframes_t adapted_sample_rate)
{
	return NULL;
}

SPA_EXPORT
int jack_destroy_adapter(jack_adapter_t* adapter)
{
	return ENOTSUP;
}

SPA_EXPORT
void jack_flush_adapter(jack_adapter_t* adapter)
{
}

SPA_EXPORT
int jack_adapter_push_and_pull(jack_adapter_t* adapter, float** input, float** output, unsigned int frames)
{
	return ENOTSUP;
}

SPA_EXPORT
int jack_adapter_pull_and_push(jack_adapter_t* adapter, float** input, float** output, unsigned int frames)
{
	return ENOTSUP;
}
