/* Simple Plugin API */
/* SPDX-FileCopyrightText: Copyright © 2024 Wim Taymans */
/* SPDX-License-Identifier: MIT */


#ifndef SPA_CONTROL_UMP_UTILS_H
#define SPA_CONTROL_UMP_UTILS_H

#include <errno.h>
#include <spa/utils/defs.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef SPA_API_CONTROL_UMP_UTILS
 #ifdef SPA_API_IMPL
  #define SPA_API_CONTROL_UMP_UTILS SPA_API_IMPL
 #else
  #define SPA_API_CONTROL_UMP_UTILS static inline
 #endif
#endif
/**
 * \addtogroup spa_control
 * \{
 */

SPA_API_CONTROL_UMP_UTILS size_t spa_ump_message_size(uint8_t message_type)
{
	static const uint32_t ump_sizes[] = {
		[0x0] = 1, /* Utility messages */
		[0x1] = 1, /* System messages */
		[0x2] = 1, /* MIDI 1.0 messages */
		[0x3] = 2, /* 7bit SysEx messages */
		[0x4] = 2, /* MIDI 2.0 messages */
		[0x5] = 4, /* 8bit data message */
		[0x6] = 1,
		[0x7] = 1,
		[0x8] = 2,
		[0x9] = 2,
		[0xa] = 2,
		[0xb] = 3,
		[0xc] = 3,
		[0xd] = 4, /* Flexible data messages */
		[0xe] = 4,
		[0xf] = 4, /* Stream messages */
	};
	return ump_sizes[message_type & 0xf];
}

SPA_API_CONTROL_UMP_UTILS int spa_ump_to_midi(const uint32_t **ump, size_t *ump_size,
		uint8_t *midi, size_t midi_maxsize, uint64_t *state)
{
	int size = 0;
	uint32_t to_consume = 0;
	const uint32_t *u = *ump;

	if (*ump_size < 4 ||
	    (to_consume = (spa_ump_message_size(u[0]>>28) * 4)) > *ump_size) {
		to_consume = *ump_size;
		goto done;
	}
	if (midi_maxsize < 8)
		return -ENOSPC;

	switch (u[0] >> 28) {
	case 0x1: /* System Real Time and System Common Messages (except System Exclusive) */
		midi[size++] = (u[0] >> 16) & 0xff;
		if (midi[0] >= 0xf1 && midi[0] <= 0xf3) {
			midi[size++] = (u[0] >> 8) & 0x7f;
			if (midi[0] == 0xf2)
				midi[size++] = u[0] & 0x7f;
		}
		break;
	case 0x2: /* MIDI 1.0 Channel Voice Messages */
		midi[size++] = (u[0] >> 16);
		midi[size++] = (u[0] >> 8);
		if (midi[0] < 0xc0 || midi[0] > 0xdf)
			midi[size++] = (u[0]);
		break;
	case 0x3: /* Data Messages (including System Exclusive) */
	{
		uint8_t status, i, bytes;

		status = (u[0] >> 20) & 0xf;
		bytes = SPA_CLAMP((u[0] >> 16) & 0xf, 0u, 6u);

		if (status == 0 || status == 1)
			midi[size++] = 0xf0;
		for (i = 0 ; i < bytes; i++)
			/* u[0] >> 8 | u[0] | u[1] >> 24 | u[1] >>16 ... */
			midi[size++] = u[(i+2)/4] >> ((5-i)%4 * 8);
		if (status == 0 || status == 3)
			midi[size++] = 0xf7;
		break;
	}
	case 0x4: /* MIDI 2.0 Channel Voice Messages */
	{
		uint8_t status = (u[0] >> 16) | 0x80;
		switch (status & 0xf0) {
		case 0xc0:
			/* program/bank change */
			if (!(u[0] & 1))
				*state = 2;
			if (*state == 0) {
				midi[size++] = (status & 0xf) | 0xb0;
				midi[size++] = 0;
				midi[size++] = (u[1] >> 8);
				to_consume = 0;
				*state = 1;
			}
			else if (*state == 1) {
				midi[size++] = (status & 0xf) | 0xb0;
				midi[size++] = 32;
				midi[size++] = u[1];
				to_consume = 0;
				*state = 2;
			}
			else if (*state == 2) {
				midi[size++] = status;
				midi[size++] = (u[1] >> 24);
				*state = 0;
			}
			break;
		default:
			midi[size++] = status;
			midi[size++] = (u[0] >> 8) & 0x7f;
			SPA_FALLTHROUGH;
		case 0xd0:
			midi[size++] = (u[1] >> 25);
			break;
		}
		break;
	}
	case 0x0: /* Utility Messages */
	case 0x5: /* Data Messages */
	default:
		break;
	}
done:
	(*ump_size) -= to_consume;
	(*ump) = SPA_PTROFF(*ump, to_consume, uint32_t);
	return size;
}

SPA_API_CONTROL_UMP_UTILS int spa_ump_from_midi(uint8_t **midi, size_t *midi_size,
		uint32_t *ump, size_t ump_maxsize, uint8_t group, uint64_t *state)
{
	int size = 0;
	uint32_t i, prefix = group << 24, to_consume = 0, bytes;
	uint8_t status, *m = (*midi), end;

	if (*midi_size < 1)
		return 0;
	if (ump_maxsize < 16)
		return -ENOSPC;

	status = m[0];

	/* SysEx */
	if (*state == 0) {
		if (status == 0xf0)
			*state = 1; /* sysex start */
		else if (status == 0xf7)
			*state = 2; /* sysex continue */
	}
	if (*state & 3) {
		prefix |= 0x30000000;
		if (status & 0x80) {
			m++;
			to_consume++;
		}
		bytes = SPA_CLAMP(*midi_size - to_consume, 0u, 7u);
		if (bytes > 0) {
			end = m[bytes-1];
			if (end & 0x80) {
				bytes--; /* skip terminator */
				to_consume++;
			}
			else
				end = 0xf0; /* pretend there is a continue terminator */

			bytes = SPA_CLAMP(bytes, 0u, 6u);
			to_consume += bytes;

			if (end == 0xf7) {
				if (*state == 2) {
					/* continue and done */
					prefix |= 0x3 << 20;
					*state = 0;
				}
			} else if (*state == 1) {
				/* first packet but not finished */
				prefix |= 0x1 << 20;
				*state = 2; /* sysex continue */
			} else {
				/* continue and not finished */
				prefix |= 0x2 << 20;
			}
			ump[size++] = prefix | bytes << 16;
			ump[size++] = 0;
			for (i = 0 ; i < bytes; i++)
				/* ump[0] |= (m[0] & 0x7f) << 8
				 * ump[0] |= (m[1] & 0x7f)
				 * ump[1] |= (m[2] & 0x7f) << 24
				 * ... */
				ump[(i+2)/4] |= (m[i] & 0x7f) << ((5-i)%4 * 8);
		}
	} else {
		/* regular messages */
		switch (status) {
		case 0x80 ... 0x8f:
		case 0x90 ... 0x9f:
		case 0xa0 ... 0xaf:
		case 0xb0 ... 0xbf:
		case 0xe0 ... 0xef:
			to_consume = 3;
			prefix |= 0x20000000;
			break;
		case 0xc0 ... 0xdf:
			to_consume = 2;
			prefix |= 0x20000000;
			break;
		case 0xf2:
			to_consume = 3;
			prefix |= 0x10000000;
			break;
		case 0xf1: case 0xf3:
			to_consume = 2;
			prefix |= 0x10000000;
			break;
		case 0xf4 ... 0xff:
			to_consume = 1;
			prefix |= 0x10000000;
			break;
		default:
			return -EIO;
		}
		if (*midi_size < to_consume) {
			to_consume = *midi_size;
		} else {
			prefix |= status << 16;
			if (to_consume > 1)
				prefix |= (m[1] & 0x7f) << 8;
			if (to_consume > 2)
				prefix |= (m[2] & 0x7f);
			ump[size++] = prefix;
		}
	}
	(*midi_size) -= to_consume;
	(*midi) += to_consume;

	return size * 4;
}

/**
 * \}
 */

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* SPA_CONTROL_UMP_UTILS_H */
