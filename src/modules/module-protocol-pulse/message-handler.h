#ifndef PULSE_SERVER_MESSAGE_HANDLER_H
#define PULSE_SERVER_MESSAGE_HANDLER_H

struct pw_manager_object;

void register_object_message_handlers(struct pw_manager_object *o);

#endif /* PULSE_SERVER_MESSAGE_HANDLER_H */
