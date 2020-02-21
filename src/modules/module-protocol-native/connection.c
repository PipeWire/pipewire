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
#include <spa/utils/result.h>
#include <spa/pod/builder.h>

#include <pipewire/pipewire.h>

#include "connection.h"

#define MAX_BUFFER_SIZE (1024 * 32)
#define MAX_FDS 1024
#define MAX_FDS_MSG 28

#define HDR_SIZE	16

static bool debug_messages = 0;

struct buffer {
	uint8_t *buffer_data;
	size_t buffer_size;
	size_t buffer_maxsize;
	int fds[MAX_FDS];
	uint32_t n_fds;

	uint32_t seq;
	size_t offset;
	size_t fds_offset;
	struct pw_protocol_native_message msg;
};

struct impl {
	struct pw_protocol_native_connection this;
	struct pw_context *context;

	struct buffer in, out;
	struct spa_pod_builder builder;

	uint32_t version;
	size_t hdr_size;
};

/** \endcond */

/** Get an fd from a connection
 *
 * \param conn the connection
 * \param index the index of the fd to get
 * \return the fd at \a index or -ENOENT when no such fd exists
 *
 * \memberof pw_protocol_native_connection
 */
int pw_protocol_native_connection_get_fd(struct pw_protocol_native_connection *conn, uint32_t index)
{
	struct impl *impl = SPA_CONTAINER_OF(conn, struct impl, this);
	struct buffer *buf = &impl->in;

	if (index == SPA_ID_INVALID)
		return -1;

	if (index >= buf->msg.n_fds)
		return -ENOENT;

	return buf->msg.fds[index];
}

/** Add an fd to a connection
 *
 * \param conn the connection
 * \param fd the fd to add
 * \return the index of the fd or SPA_IDX_INVALID when an error occured
 *
 * \memberof pw_protocol_native_connection
 */
uint32_t pw_protocol_native_connection_add_fd(struct pw_protocol_native_connection *conn, int fd)
{
	struct impl *impl = SPA_CONTAINER_OF(conn, struct impl, this);
	struct buffer *buf = &impl->out;
	uint32_t index, i;

	if (fd < 0)
		return SPA_IDX_INVALID;

	for (i = 0; i < buf->msg.n_fds; i++) {
		if (buf->msg.fds[i] == fd)
			return i;
	}

	index = buf->msg.n_fds;
	if (index + buf->n_fds >= MAX_FDS) {
		pw_log_error("connection %p: too many fds (%d)", conn, MAX_FDS);
		return SPA_IDX_INVALID;
	}

	buf->msg.fds[index] = fd;
	buf->msg.n_fds++;
	pw_log_debug("connection %p: add fd %d at index %d", conn, fd, index);

	return index;
}

static void *connection_ensure_size(struct pw_protocol_native_connection *conn, struct buffer *buf, size_t size)
{
	int res;

	if (buf->buffer_size + size > buf->buffer_maxsize) {
		buf->buffer_maxsize = SPA_ROUND_UP_N(buf->buffer_size + size, MAX_BUFFER_SIZE);
		buf->buffer_data = realloc(buf->buffer_data, buf->buffer_maxsize);
		if (buf->buffer_data == NULL) {
			res = -errno;
			buf->buffer_maxsize = 0;
			spa_hook_list_call(&conn->listener_list,
					struct pw_protocol_native_connection_events,
					error, 0, -res);
			errno = -res;
			return NULL;
		}
		pw_log_debug("connection %p: resize buffer to %zd %zd %zd",
			    conn, buf->buffer_size, size, buf->buffer_maxsize);
	}
	return (uint8_t *) buf->buffer_data + buf->buffer_size;
}

static int refill_buffer(struct pw_protocol_native_connection *conn, struct buffer *buf)
{
	ssize_t len;
	struct cmsghdr *cmsg;
	struct msghdr msg = { 0 };
	struct iovec iov[1];
	char cmsgbuf[CMSG_SPACE(MAX_FDS_MSG * sizeof(int))];
	int n_fds = 0;
	size_t avail;

	avail = buf->buffer_maxsize - buf->buffer_size;

	iov[0].iov_base = buf->buffer_data + buf->buffer_size;
	iov[0].iov_len = avail;
	msg.msg_iov = iov;
	msg.msg_iovlen = 1;
	msg.msg_control = cmsgbuf;
	msg.msg_controllen = sizeof(cmsgbuf);
	msg.msg_flags = MSG_CMSG_CLOEXEC | MSG_DONTWAIT;

	while (true) {
		len = recvmsg(conn->fd, &msg, msg.msg_flags);
		if (len == 0 && avail != 0)
			return -EPIPE;
		else if (len < 0) {
			if (errno == EINTR)
				continue;
			if (errno != EAGAIN || errno != EWOULDBLOCK)
				goto recv_error;
			return -EAGAIN;
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

	return 0;

	/* ERRORS */
recv_error:
	pw_log_error("connection %p: could not recvmsg on fd:%d: %m", conn, conn->fd);
	return -errno;
}

static void clear_buffer(struct buffer *buf)
{
	buf->n_fds = 0;
	buf->buffer_size = 0;
	buf->offset = 0;
	buf->fds_offset = 0;
}

/** Make a new connection object for the given socket
 *
 * \param fd the socket
 * \returns a newly allocated connection object
 *
 * \memberof pw_protocol_native_connection
 */
struct pw_protocol_native_connection *pw_protocol_native_connection_new(struct pw_context *context, int fd)
{
	struct impl *impl;
	struct pw_protocol_native_connection *this;

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		return NULL;

	debug_messages = pw_debug_is_category_enabled("connection");
	impl->context = context;

	this = &impl->this;

	pw_log_debug("connection %p: new", this);

	this->fd = fd;
	spa_hook_list_init(&this->listener_list);

	impl->hdr_size = HDR_SIZE;
	impl->version = 3;

	impl->out.buffer_data = calloc(1, MAX_BUFFER_SIZE);
	impl->out.buffer_maxsize = MAX_BUFFER_SIZE;
	impl->in.buffer_data = calloc(1, MAX_BUFFER_SIZE);
	impl->in.buffer_maxsize = MAX_BUFFER_SIZE;

	if (impl->out.buffer_data == NULL || impl->in.buffer_data == NULL)
		goto no_mem;

	return this;

no_mem:
	free(impl->out.buffer_data);
	free(impl->in.buffer_data);
	free(impl);
	return NULL;
}

int pw_protocol_native_connection_set_fd(struct pw_protocol_native_connection *conn, int fd)
{
	conn->fd = fd;
	return 0;
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

static int prepare_packet(struct pw_protocol_native_connection *conn, struct buffer *buf)
{
	struct impl *impl = SPA_CONTAINER_OF(conn, struct impl, this);
	uint8_t *data;
	size_t size, len;
	uint32_t *p;

	data = buf->buffer_data + buf->offset;
	size = buf->buffer_size - buf->offset;

	if (size < impl->hdr_size)
		return impl->hdr_size;

	p = (uint32_t *) data;

	buf->msg.id = p[0];
	buf->msg.opcode = p[1] >> 24;
	len = p[1] & 0xffffff;

	if (buf->msg.id == 0 && buf->msg.opcode == 1) {
		if (p[3] >= 4) {
			pw_log_warn("old version detected");
			impl->version = 0;
			impl->hdr_size = 8;
		} else {
			impl->version = 3;
			impl->hdr_size = 16;
		}
		spa_hook_list_call(&conn->listener_list,
				struct pw_protocol_native_connection_events,
				start, 0, impl->version);
	}

	if (impl->version >= 3) {
		buf->msg.seq = p[2];
		buf->msg.n_fds = p[3];
	} else {
		buf->msg.seq = 0;
		buf->msg.n_fds = 0;
	}
	data += impl->hdr_size;
	size -= impl->hdr_size;
	buf->msg.fds = &buf->fds[buf->fds_offset];

	if (size < len)
		return len;

	buf->msg.size = len;
	buf->msg.data = data;

	buf->offset += impl->hdr_size + len;
	buf->fds_offset += buf->msg.n_fds;

	if (buf->offset >= buf->buffer_size)
		clear_buffer(buf);

	return 0;
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
int
pw_protocol_native_connection_get_next(struct pw_protocol_native_connection *conn,
		const struct pw_protocol_native_message **msg)
{
	struct impl *impl = SPA_CONTAINER_OF(conn, struct impl, this);
	int len, res;
	struct buffer *buf;

	buf = &impl->in;

	while (1) {
		len = prepare_packet(conn, buf);
		if (len < 0)
			return len;
		if (len == 0)
			break;

		if (connection_ensure_size(conn, buf, len) == NULL)
			return -errno;
		if ((res = refill_buffer(conn, buf)) < 0)
			return res;
	}
	*msg = &buf->msg;
	return 1;
}

static inline void *begin_write(struct pw_protocol_native_connection *conn, uint32_t size)
{
	struct impl *impl = SPA_CONTAINER_OF(conn, struct impl, this);
	uint32_t *p;
	struct buffer *buf = &impl->out;
	/* header and size for payload */
	if ((p = connection_ensure_size(conn, buf, impl->hdr_size + size)) == NULL)
		return NULL;

	return SPA_MEMBER(p, impl->hdr_size, void);
}

static int builder_overflow(void *data, uint32_t size)
{
	struct impl *impl = data;
	struct spa_pod_builder *b = &impl->builder;

	b->size = SPA_ROUND_UP_N(size, 4096);
	if ((b->data = begin_write(&impl->this, b->size)) == NULL)
		return -errno;
        return 0;
}

static const struct spa_pod_builder_callbacks builder_callbacks = {
	SPA_VERSION_POD_BUILDER_CALLBACKS,
	.overflow = builder_overflow
};

struct spa_pod_builder *
pw_protocol_native_connection_begin(struct pw_protocol_native_connection *conn,
			uint32_t id, uint8_t opcode,
			struct pw_protocol_native_message **msg)
{
	struct impl *impl = SPA_CONTAINER_OF(conn, struct impl, this);
	struct buffer *buf = &impl->out;

	buf->msg.id = id;
	buf->msg.opcode = opcode;
	impl->builder = SPA_POD_BUILDER_INIT(NULL, 0);
	spa_pod_builder_set_callbacks(&impl->builder, &builder_callbacks, impl);
	if (impl->version >= 3) {
		buf->msg.n_fds = 0;
		buf->msg.fds = &buf->fds[buf->n_fds];
	} else {
		buf->msg.n_fds = buf->n_fds;
		buf->msg.fds = &buf->fds[0];
	}

	buf->msg.seq = buf->seq;
	if (msg)
		*msg = &buf->msg;
	return &impl->builder;
}

int
pw_protocol_native_connection_end(struct pw_protocol_native_connection *conn,
				  struct spa_pod_builder *builder)
{
	struct impl *impl = SPA_CONTAINER_OF(conn, struct impl, this);
	uint32_t *p, size = builder->state.offset;
	struct buffer *buf = &impl->out;
	int res;

	if ((p = connection_ensure_size(conn, buf, impl->hdr_size + size)) == NULL)
		return -errno;

	p[0] = buf->msg.id;
	p[1] = (buf->msg.opcode << 24) | (size & 0xffffff);
	if (impl->version >= 3) {
		p[2] = buf->msg.seq;
		p[3] = buf->msg.n_fds;
	}

	buf->buffer_size += impl->hdr_size + size;
	if (impl->version >= 3)
		buf->n_fds += buf->msg.n_fds;
	else
		buf->n_fds = buf->msg.n_fds;

	if (debug_messages) {
		fprintf(stderr, ">>>>>>>>> out: id:%d op:%d size:%d seq:%d\n",
				buf->msg.id, buf->msg.opcode, size, buf->msg.seq);
	        spa_debug_pod(0, NULL, SPA_MEMBER(p, impl->hdr_size, struct spa_pod));
	}

	buf->seq = (buf->seq + 1) & SPA_ASYNC_SEQ_MASK;
	res = SPA_RESULT_RETURN_ASYNC(buf->msg.seq);

	spa_hook_list_call(&conn->listener_list,
			struct pw_protocol_native_connection_events, need_flush, 0);

	return res;
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
	int res = 0, *fds;
	uint32_t fds_len, n_fds, outfds;
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
			memcpy(CMSG_DATA(cmsg), fds, fds_len);
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
				else {
					res = -errno;
					goto exit;
				}
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

	res = 0;

exit:
	if (size > 0)
		memmove(buf->buffer_data, data, size);
	buf->buffer_size = size;
	if (n_fds > 0)
		memmove(buf->fds, fds, n_fds * sizeof(int));
	buf->n_fds = n_fds;
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

	return 0;
}
