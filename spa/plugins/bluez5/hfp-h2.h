/* Spa HFP Codecs */
/* SPDX-FileCopyrightText: Copyright © 2025 Pauli Virtanen */
/* SPDX-License-Identifier: MIT */

#ifndef SPA_BLUEZ5_HFP_H2_H
#define SPA_BLUEZ5_HFP_H2_H

#define H2_PACKET_SIZE		60

struct h2_reader {
	uint8_t buf[H2_PACKET_SIZE];
	uint8_t pos;
	bool msbc;
	uint16_t seq;
	bool started;
};

struct h2_writer {
	uint8_t seq;
};

static inline void h2_reader_init(struct h2_reader *this, bool msbc)
{
	this->pos = 0;
	this->msbc = msbc;
	this->seq = 0;
	this->started = false;
}

static inline void h2_reader_append_byte(struct h2_reader *this, uint8_t byte)
{
        /* Parse H2 sync header */
        if (this->pos == 0) {
                if (byte != 0x01) {
                        this->pos = 0;
                        return;
                }
        } else if (this->pos == 1) {
                if (!((byte & 0x0F) == 0x08 &&
                      ((byte >> 4) & 1) == ((byte >> 5) & 1) &&
                      ((byte >> 6) & 1) == ((byte >> 7) & 1))) {
                        this->pos = 0;
                        return;
                }
        } else if (this->msbc) {
		/* Beginning of MSBC frame: SYNCWORD + 2 nul bytes */
		if (this->pos == 2) {
			if (byte != 0xAD) {
				this->pos = 0;
				return;
			}
		}
		else if (this->pos == 3) {
			if (byte != 0x00) {
				this->pos = 0;
				return;
			}
		}
		else if (this->pos == 4) {
			if (byte != 0x00) {
				this->pos = 0;
				return;
			}
                }
	}

	if (this->pos >= H2_PACKET_SIZE) {
		/* Packet completed. Reset. */
		this->pos = 0;
		h2_reader_append_byte(this, byte);
		return;
	}

	this->buf[this->pos] = byte;
	++this->pos;
}

static inline void *h2_reader_read(struct h2_reader *this, const uint8_t *src, size_t src_size, size_t *consumed, size_t *avail)
{
	int seq;
	size_t i;

	for (i = 0; i < src_size && this->pos < H2_PACKET_SIZE; ++i)
		h2_reader_append_byte(this, src[i]);

	*consumed = i;
	*avail = 0;

	if (this->pos < H2_PACKET_SIZE)
		return NULL;

	this->pos = 0;

	seq = ((this->buf[1] >> 4) & 1) | ((this->buf[1] >> 6) & 2);
	if (!this->started) {
		this->seq = seq;
		this->started = true;
	}

	this->seq++;
	while (seq != this->seq % 4)
		this->seq++;

	*avail = H2_PACKET_SIZE - 2;
	return &this->buf[2];
}

static inline void h2_write(uint8_t *buf, uint8_t seq)
{
	static const uint8_t sntable[4] = { 0x08, 0x38, 0xc8, 0xf8 };

	buf[0] = 0x01;
	buf[1] = sntable[seq % 4];
	buf[59] = 0;
}

static inline bool is_zero_packet(const uint8_t *data, size_t size)
{
	size_t i;

	for (i = 0; i < size; ++i)
		if (data[i])
			return false;

	return true;
}

#endif
