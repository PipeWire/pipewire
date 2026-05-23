/* SPDX-FileCopyrightText: Copyright © 2026 Nils Tonnaett
 * Nils Tonnaett <ntonnatt@ccrma.stanford.edu> */
/* SPDX-License-Identifier: MIT */

#include "strings.h"

#include <stdbool.h>

typedef enum {
	ST_START,
	ST_A,
	ST_B,
	ST_C,
	ST_D,
	ST_E,
	ST_F,
	ST_G,
} UTF8_STATE;

/*
 * IEEE 1722.1 Section 7.4.17.1
 *
 * We need to check if the buffer str of length len is valid UTF-8.
 * The algorithm implemented here is based on the state machine by Frank Yung-Fong Tang
 * described here at
 * https://unicode.org/mail-arch/unicode-ml/y2003-m02/att-0467/01-The_Algorithm_to_Valide_an_UTF-8_String
 */
int validate_utf8 (const unsigned char *str, size_t len)
{
	UTF8_STATE state = ST_START;
	bool err = false;

	for (unsigned int i = 0; i < len; ++i)
	{
		switch (state)
		{
		case ST_START:
			if (str[i] <= 0x7F)
			{
				continue;
			}
			else if (str[i] >= 0xC2 && str[i] <= 0xDF)
			{
				state = ST_A;
			}
			else if (str[i] >= 0xE1 && str[i] <= 0xEC)
			{
				state = ST_B;
			}
			else if (str[i] >= 0xEE && str[i] <= 0xEF)
			{
				state = ST_B;
			}
			else if (str[i] == 0xE0)
			{
				state = ST_C;
			}
			else if (str[i] == 0xED)
			{
				state = ST_D;
			}
			else if (str[i] >= 0xF1 && str[i] <= 0xF3)
			{
				state = ST_E;
			}
			else if (str[i] == 0xF0)
			{
				state = ST_F;
			}
			else if (str[i] >= 0xF4)
			{
				state = ST_G;
			}
			else
			{
				err = true;
			}
			break;
		case ST_A:
			if (str[i] >= 0x80 && str[i] <= 0xBF)
				state =  ST_START;
			else
				err = true;
			break;
		case ST_B:
			if (str[i] >= 0x80 && str[i] <= 0xBF)
				state =  ST_A;
			else
				err = true;
			break;
		case ST_C:
			if (str[i] >= 0xA0 && str[i] <= 0xBF)
				state =  ST_A;
			else
				err = true;
			break;
		case ST_D:
			if (str[i] >= 0x80 && str[i] <= 0x9F)
				state =  ST_A;
			else
				err = true;
			break;
		case ST_E:
			if (str[i] >= 0x80 && str[i] <= 0xBF)
				state =  ST_B;
			else
				err = true;
			break;
		case ST_F:
			if (str[i] >= 0x90 && str[i] <= 0xBF)
				state =  ST_B;
			else
				err = true;
			break;
		case ST_G:
			if (str[i] >= 0x80 && str[i] <= 0x8F)
				state =  ST_B;
			else
				err = true;
			break;
		}
		if (err == true)
		{
			return -1;
		}
	}
	if (state != ST_START)
	{
		return -1;
	}
	else
	{
		return 0;
	}
}

/*
 * For SET_NAME, strings need to be zero-padded if shorter than 64 bytes. A
 * string of 64 bytes would NOT be nul-terminated.
 *
 * IEEE 1722.1 Section 7.4.17.1
 */
int check_zero_padding (const unsigned char *str, size_t len)
{
	size_t str_len = strnlen ((char *)str, len);
	/* String doesn't need to be null-terminated. Return success if there is no
	* null in str */
	if (str_len == len)
	{
		return 0;
	}

	for (unsigned int i = str_len; i < len; ++i)
	{
		if (str[i] != 0x00)
		{
			return -1;
		}
	}

	return 0;
}
