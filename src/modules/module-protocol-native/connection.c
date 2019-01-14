/* PipeWire
 *
 * Copyright © 2018 Wim Taymans
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

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>

#include <spa/debug/pod.h>

#include <pipewire/pipewire.h>
#include "pipewire/private.h"

#include "connection.h"

#define MAX_BUFFER_SIZE (1024 * 32)
#define MAX_FDS 1024
#define MAX_FDS_MSG 28

static bool debug_messages = 0;

struct buffer {
	uint8_t *buffer_data;
	size_t buffer_size;
	size_t buffer_maxsize;
	int fds[MAX_FDS];
	uint32_t n_fds;

	size_t offset;
	void *data;
	size_t size;

	bool update;
};

struct impl {
	struct pw_protocol_native_connection this;

	struct buffer in, out;

	uint32_t dest_id;
	uint8_t opcode;
	struct spa_pod_builder builder;

	struct pw_core *core;
};

/** \endcond */

/** Get an fd from a connection
 *
 * \param conn the connection
 * \param index the index of the fd to get
 * \return the fd at \a index or -1 when no such fd exists
 *
 * \memberof pw_protocol_native_connection
 */
int pw_protocol_native_connection_get_fd(struct pw_protocol_native_connection *conn, uint32_t index)
{
	struct impl *impl = SPA_CONTAINER_OF(conn, struct impl, this);

	if (index >= impl->in.n_fds)
		return -1;

	return impl->in.fds[index];
}

/** Add an fd to a connection
 *
 * \param conn the connection
 * \param fd the fd to add
 * \return the index of the fd or -1 when an error occured
 *
 * \memberof pw_protocol_native_connection
 */
uint32_t pw_protocol_native_connection_add_fd(struct pw_protocol_native_connection *conn, int fd)
{
	struct impl *impl = SPA_CONTAINER_OF(conn, struct impl, this);
	uint32_t index, i;

	for (i = 0; i < impl->out.n_fds; i++) {
		if (impl->out.fds[i] == fd)
			return i;
	}

	index = impl->out.n_fds;
	if (index >= MAX_FDS) {
		pw_log_error("connection %p: too many fds", conn);
		return -1;
	}

	impl->out.fds[index] = fd;
	impl->out.n_fds++;

	return index;
}

static void *connection_ensure_size(struct pw_protocol_native_connection *conn, struct buffer *buf, size_t size)
{
	if (buf->buffer_size + size > buf->buffer_maxsize) {
		buf->buffer_maxsize = SPA_ROUND_UP_N(buf->buffer_size + size, MAX_BUFFER_SIZE);
		buf->buffer_data = realloc(buf->buffer_data, buf->buffer_maxsize);
		if (buf->buffer_data == NULL) {
			buf->buffer_maxsize = 0;
			spa_hook_list_call(&conn->listener_list, struct pw_protocol_native_connection_events, error, 0, -ENOMEM);
			return NULL;
		}
		pw_log_warn("connection %p: resize buffer to %zd %zd %zd",
			    conn, buf->buffer_size, size, buf->buffer_maxsize);
	}
	return (uint8_t *) buf->buffer_data + buf->buffer_size;
}

static bool refill_buffer(struct pw_protocol_native_connection *conn, struct buffer *buf)
{
	ssize_t len;
	struct cmsghdr *cmsg;
	struct msghdr msg = { 0 };
	struct iovec iov[1];
	char cmsgbuf[CMSG_SPACE(MAX_FDS_MSG * sizeof(int))];
	int n_fds = 0;

	iov[0].iov_base = buf->buffer_data + buf->buffer_size;
	iov[0].iov_len = buf->buffer_maxsize - buf->buffer_size;
	msg.msg_iov = iov;
	msg.msg_iovlen = 1;
	msg.msg_control = cmsgbuf;
	msg.msg_controllen = sizeof(cmsgbuf);
	msg.msg_flags = MSG_CMSG_CLOEXEC | MSG_DONTWAIT;

	while (true) {
		len = recvmsg(conn->fd, &msg, msg.msg_flags);
		if (len < 0) {
			if (errno == EINTR)
				continue;
			if (errno != EAGAIN || errno != EWOULDBLOCK)
				goto recv_error;
			return false;
		}
		break;
	}

	buf->buffer_size += len;

	/* handle control messages */
	for (cmsg = CMSG_FIRSTHDR(&msg); cmsg != NULL; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
		if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS)
			continue;

		n_fds =
		    (cmsg->cmsg_len - ((char *) CMSG_DATA(cmsg) - (char *) cmsg)) / sizeof(int);
		memcpy(&buf->fds[buf->n_fds], CMSG_DATA(cmsg), n_fds * sizeof(int));
		buf->n_fds += n_fds;
	}
	pw_log_trace("connection %p: %d read %zd bytes and %d fds", conn, conn->fd, len,
		     n_fds);

	return true;

	/* ERRORS */
      recv_error:
	pw_log_error("could not recvmsg on fd %d: %s", conn->fd, strerror(errno));
	return false;
}

static void clear_buffer(struct buffer *buf)
{
	buf->n_fds = 0;
	buf->offset = 0;
	buf->size = 0;
	buf->buffer_size = 0;
}

/** Make a new connection object for the given socket
 *
 * \param fd the socket
 * \returns a newly allocated connection object
 *
 * \memberof pw_protocol_native_connection
 */
struct pw_protocol_native_connection *pw_protocol_native_connection_new(struct pw_core *core, int fd)
{
	struct impl *impl;
	struct pw_protocol_native_connection *this;

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		return NULL;

	debug_messages = pw_debug_is_category_enabled("connection");

	this = &impl->this;

	pw_log_debug("connection %p: new", this);

	this->fd = fd;
	spa_hook_list_init(&this->listener_list);

	impl->out.buffer_data = malloc(MAX_BUFFER_SIZE);
	impl->out.buffer_maxsize = MAX_BUFFER_SIZE;
	impl->in.buffer_data = malloc(MAX_BUFFER_SIZE);
	impl->in.buffer_maxsize = MAX_BUFFER_SIZE;
	impl->in.update = true;
	impl->core = core;

	if (impl->out.buffer_data == NULL || impl->in.buffer_data == NULL)
		goto no_mem;

	return this;

      no_mem:
	free(impl->out.buffer_data);
	free(impl->in.buffer_data);
	free(impl);
	return NULL;
}

/** Destroy a connection
 *
 * \param conn the connection to destroy
 *
 * \memberof pw_protocol_native_connection
 */
void pw_protocol_native_connection_destroy(struct pw_protocol_native_connection *conn)
{
	struct impl *impl = SPA_CONTAINER_OF(conn, struct impl, this);

	pw_log_debug("connection %p: destroy", conn);

	spa_hook_list_call(&conn->listener_list, struct pw_protocol_native_connection_events, destroy, 0);

	free(impl->out.buffer_data);
	free(impl->in.buffer_data);
	free(impl);
}

/** Move to the next packet in the connection
 *
 * \param conn the connection
 * \param opcode addres of result opcode
 * \param dest_id addres of result destination id
 * \param dt pointer to packet data
 * \param sz size of packet data
 * \return true on success
 *
 * Get the next packet in \a conn and store the opcode and destination
 * id as well as the packet data and size.
 *
 * \memberof pw_protocol_native_connection
 */
bool
pw_protocol_native_connection_get_next(struct pw_protocol_native_connection *conn,
		       uint8_t *opcode,
		       uint32_t *dest_id,
		       void **dt,
		       uint32_t *sz)
{
	struct impl *impl = SPA_CONTAINER_OF(conn, struct impl, this);
	size_t len, size;
	uint8_t *data;
	struct buffer *buf;
	uint32_t *p;

	buf = &impl->in;

	/* move to next packet */
	buf->offset += buf->size;

      again:
	if (buf->update) {
		if (!refill_buffer(conn, buf))
			return false;
		buf->update = false;
	}

	/* now read packet */
	data = buf->buffer_data;
	size = buf->buffer_size;

	if (buf->offset >= size) {
		clear_buffer(buf);
		buf->update = true;
		return false;
	}

	data += buf->offset;
	size -= buf->offset;

	if (size < 8) {
		if (connection_ensure_size(conn, buf, 8) == NULL)
			return false;
		buf->update = true;
		goto again;
	}
	p = (uint32_t *) data;
	data += 8;
	size -= 8;

	*dest_id = p[0];
	*opcode = p[1] >> 24;
	len = p[1] & 0xffffff;

	if (len > size) {
		if (connection_ensure_size(conn, buf, len) == NULL)
			return false;
		buf->update = true;
		goto again;
	}
	buf->size = len;
	buf->data = data;
	buf->offset += 8;

	*dt = buf->data;
	*sz = buf->size;

	return true;
}

static inline void *begin_write(struct pw_protocol_native_connection *conn, uint32_t size)
{
	struct impl *impl = SPA_CONTAINER_OF(conn, struct impl, this);
	uint32_t *p;
	struct buffer *buf = &impl->out;
	/* 4 for dest_id, 1 for opcode, 3 for size and size for payload */
	if ((p = connection_ensure_size(conn, buf, 8 + size)) == NULL)
		return NULL;

	return p + 2;
}

static uint32_t write_pod(struct spa_pod_builder *b, const void *data, uint32_t size)
{
	struct impl *impl = SPA_CONTAINER_OF(b, struct impl, builder);
	uint32_t ref = b->state.offset;

        if (b->size <= ref) {
                b->size = SPA_ROUND_UP_N(ref + size, 4096);
                b->data = begin_write(&impl->this, b->size);
        }
        memcpy(SPA_MEMBER(b->data, ref, void), data, size);

        return ref;
}

struct spa_pod_builder *
pw_protocol_native_connection_begin_resource(struct pw_protocol_native_connection *conn,
					     struct pw_resource *resource,
					     uint8_t opcode)
{
	struct impl *impl = SPA_CONTAINER_OF(conn, struct impl, this);

	impl->dest_id = resource->id;
	impl->opcode = opcode;
	impl->builder = (struct spa_pod_builder) { NULL, 0, write_pod };

	return &impl->builder;
}

struct spa_pod_builder *
pw_protocol_native_connection_begin_proxy(struct pw_protocol_native_connection *conn,
					  struct pw_proxy *proxy,
					  uint8_t opcode)
{
	struct impl *impl = SPA_CONTAINER_OF(conn, struct impl, this);

	impl->dest_id = proxy->id;
	impl->opcode = opcode;
	impl->builder = (struct spa_pod_builder) { NULL, 0, write_pod, };

	return &impl->builder;
}

void
pw_protocol_native_connection_end(struct pw_protocol_native_connection *conn,
				  struct spa_pod_builder *builder)
{
	struct impl *impl = SPA_CONTAINER_OF(conn, struct impl, this);
	uint32_t *p, size = builder->state.offset;
	struct buffer *buf = &impl->out;

	if ((p = connection_ensure_size(conn, buf, 8 + size)) == NULL)
		return;

	*p++ = impl->dest_id;
	*p++ = (impl->opcode << 24) | (size & 0xffffff);

	buf->buffer_size += 8 + size;

	if (debug_messages) {
		fprintf(stderr, ">>>>>>>>> out: %d %d %d\n", impl->dest_id, impl->opcode, size);
	        spa_debug_pod(0, NULL, (struct spa_pod *)p);
	}
	spa_hook_list_call(&conn->listener_list,
			struct pw_protocol_native_connection_events, need_flush, 0);
}

/** Flush the connection object
 *
 * \param conn the connection object
 * \return 0 on success < 0 error code on error
 *
 * Write the queued messages on the connection to the socket
 *
 * \memberof pw_protocol_native_connection
 */
int pw_protocol_native_connection_flush(struct pw_protocol_native_connection *conn)
{
	struct impl *impl = SPA_CONTAINER_OF(conn, struct impl, this);
	ssize_t sent, outsize;
	struct msghdr msg = { 0 };
	struct iovec iov[1];
	struct cmsghdr *cmsg;
	char cmsgbuf[CMSG_SPACE(MAX_FDS_MSG * sizeof(int))];
	int *cm, res = 0, *fds;
	uint32_t i, fds_len, n_fds, outfds;
	struct buffer *buf;
	void *data;
	size_t size;

	buf = &impl->out;
	data = buf->buffer_data;
	size = buf->buffer_size;
	fds = buf->fds;
	n_fds = buf->n_fds;

	while (size > 0) {
		if (n_fds > MAX_FDS_MSG) {
			outfds = MAX_FDS_MSG;
			outsize = SPA_MIN(sizeof(uint32_t), size);
		} else {
			outfds = n_fds;
			outsize = size;
		}

		fds_len = outfds * sizeof(int);

		iov[0].iov_base = data;
		iov[0].iov_len = outsize;
		msg.msg_iov = iov;
		msg.msg_iovlen = 1;

		if (outfds > 0) {
			msg.msg_control = cmsgbuf;
			msg.msg_controllen = CMSG_SPACE(fds_len);
			cmsg = CMSG_FIRSTHDR(&msg);
			cmsg->cmsg_level = SOL_SOCKET;
			cmsg->cmsg_type = SCM_RIGHTS;
			cmsg->cmsg_len = CMSG_LEN(fds_len);
			cm = (int *) CMSG_DATA(cmsg);
			for (i = 0; i < outfds; i++)
				cm[i] = fds[i] > 0 ? fds[i] : -fds[i];
			msg.msg_controllen = cmsg->cmsg_len;
		} else {
			msg.msg_control = NULL;
			msg.msg_controllen = 0;
		}

		while (true) {
			sent = sendmsg(conn->fd, &msg, MSG_NOSIGNAL | MSG_DONTWAIT);
			if (sent < 0) {
				if (errno == EINTR)
					continue;
				else
					goto send_error;
			}
			break;
		}
		pw_log_trace("connection %p: %d written %zd bytes and %u fds", conn, conn->fd, sent,
			     outfds);

		size -= sent;
		data = SPA_MEMBER(data, sent, void);
		n_fds -= outfds;
		fds += outfds;
	}
	buf->buffer_size = size;
	buf->n_fds = n_fds;

	return 0;

	/* ERRORS */
      send_error:
	res = -errno;
	pw_log_error("could not sendmsg: %s", strerror(errno));
	return res;
}

/** Clear the connection object
 *
 * \param conn the connection object
 * \return 0 on success
 *
 * Remove all queued messages from \a conn
 *
 * \memberof pw_protocol_native_connection
 */
int pw_protocol_native_connection_clear(struct pw_protocol_native_connection *conn)
{
	struct impl *impl = SPA_CONTAINER_OF(conn, struct impl, this);

	clear_buffer(&impl->out);
	clear_buffer(&impl->in);
	impl->in.update = true;

	return 0;
}
