#ifndef PIPEWIRE_PULSE_EXTENSION_REGISTRY_H
#define PIPEWIRE_PULSE_EXTENSION_REGISTRY_H

#include <stdint.h>

struct client;
struct message;

int do_extension_stream_restore(struct client *client, uint32_t tag, struct message *m);
int do_extension_device_restore(struct client *client, uint32_t tag, struct message *m);
int do_extension_device_manager(struct client *client, uint32_t tag, struct message *m);

#endif /* PIPEWIRE_PULSE_EXTENSION_REGISTRY_H */
