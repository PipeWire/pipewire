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

struct jack_synchro {
	char name[SYNC_MAX_NAME_SIZE];
        bool flush;
	sem_t *semaphore;
};

#define JACK_SYNCHRO_INIT	(struct jack_synchro) { { 0, }, false, NULL }

static inline int
jack_synchro_init(struct jack_synchro *synchro,
		  const char *client_name,
		  const char *server_name,
		  int value, bool internal,
		  bool promiscuous)
{
	if (promiscuous)
		snprintf(synchro->name, sizeof(synchro->name),
				"jack_sem.%s_%s", server_name, client_name);
	else
		snprintf(synchro->name, sizeof(synchro->name),
				"jack_sem.%d_%s_%s", getuid(), server_name, client_name);

	synchro->flush = false;
	if ((synchro->semaphore = sem_open(synchro->name, O_CREAT | O_RDWR, 0777, value)) == (sem_t*)SEM_FAILED) {
		pw_log_error("can't check in named semaphore name = %s err = %s", synchro->name, strerror(errno));
		return -1;
	}
	return 0;
}
