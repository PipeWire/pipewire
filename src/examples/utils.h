/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 Red Hat */
/* SPDX-License-Identifier: MIT */

static inline char *
encode_hex(const uint8_t *data, size_t size)
{
	FILE *ms;
	char *encoded = NULL;
	size_t encoded_size = 0;
	size_t i;

	ms = open_memstream(&encoded, &encoded_size);
	for (i = 0; i < size; i++) {
		fprintf(ms, "%02x", data[i]);
	}
	fclose(ms);

	return encoded;
}

static inline int8_t
ascii_hex_to_hex(uint8_t ascii_hex)
{
	if (ascii_hex >= '0' && ascii_hex <= '9')
		return ascii_hex - '0';
	else if (ascii_hex >= 'a' && ascii_hex <= 'f')
		return ascii_hex - 'a' + 10;
	else if (ascii_hex >= 'A' && ascii_hex <= 'F')
		return ascii_hex - 'A' + 10;
	else
		return -1;
}

static inline int
decode_hex(const char *encoded, uint8_t *data, size_t size)
{
	size_t length;
	size_t i;

	length = strlen(encoded);

	if (size < (length / 2) * sizeof(uint8_t))
		return -1;

	i = 0;
	while (i < length) {
		int8_t top = ascii_hex_to_hex(encoded[i]);
		int8_t bottom = ascii_hex_to_hex(encoded[i + 1]);

		if (top == -1 || bottom == -1)
			return -1;

		uint8_t el = top << 4 | bottom;
		data[i / 2] = el;
		i += 2;
	}

	return 1;
}
