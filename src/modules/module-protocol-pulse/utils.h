/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2020 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef PULSE_SERVER_UTILS_H
#define PULSE_SERVER_UTILS_H

#include <stddef.h>
#include <sys/types.h>

struct client;
struct pw_context;

int get_runtime_dir(char *buf, size_t buflen);
int check_flatpak(struct client *client, pid_t pid);
pid_t get_client_pid(struct client *client, int client_fd);
const char *get_server_name(struct pw_context *context);
int create_pid_file(void);

#endif /* PULSE_SERVER_UTILS_H */
