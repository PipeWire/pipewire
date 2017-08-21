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

#include <semaphore.h>

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
		  int value,
		  bool promiscuous)
{
	char cname[SYNC_MAX_NAME_SIZE+1];
	int i;
	for (i = 0; client_name[i] != '\0'; i++) {
		if (client_name[i] == '/' || client_name[i] == '\\')
			cname[i] = '_';
		else
			cname[i] = client_name[i];
	}
	cname[i] = client_name[i];

	if (promiscuous)
		snprintf(synchro->name, sizeof(synchro->name),
				"jack_sem.%s_%s", server_name, cname);
	else
		snprintf(synchro->name, sizeof(synchro->name),
				"jack_sem.%d_%s_%s", getuid(), server_name, cname);

	synchro->flush = false;
	if ((synchro->semaphore = sem_open(synchro->name, O_CREAT | O_RDWR, 0777, value)) == (sem_t*)SEM_FAILED) {
		pw_log_error("can't check semaphore %s: %s", synchro->name, strerror(errno));
		return -1;
	}
	return 0;
}

static inline bool
jack_synchro_close(struct jack_synchro *synchro)
{
	if (synchro->semaphore == NULL)
		return true;

	if (sem_close(synchro->semaphore) < 0) {
		pw_log_warn("can't close semaphore %s: %s", synchro->name, strerror(errno));
	}
	synchro->semaphore = NULL;
	return true;
}

static inline bool
jack_synchro_signal(struct jack_synchro *synchro)
{
	int res;
	if (synchro->flush)
		return true;
	if ((res = sem_post(synchro->semaphore)) < 0)
		pw_log_error("semaphore %s post err = %s", synchro->name, strerror(errno));

	return res == 0;
}

static inline bool
jack_synchro_wait(struct jack_synchro *synchro)
{
	int res;
	while ((res = sem_wait(synchro->semaphore)) < 0) {
		if (errno != EINTR)
			continue;

		pw_log_error("semaphore %s wait err = %s", synchro->name, strerror(errno));
		break;
	}
	return res == 0;
}
