/* PipeWire */
/* SPDX-License-Identifier: MIT */
/*
 * HomeKit TLV8 encode/decode for AirPlay 2 pairing.
 *
 * Derived from HomeKit Accessory Protocol (HAP) TLV8: each
 * item is type(1 byte) || length(1 byte) || value(length bytes). A value longer
 * than 255 bytes is split into consecutive fragments of the SAME type, each
 * <=255 (a 255-byte fragment signals "more follows"). On parse, consecutive
 * items of the same type are concatenated into one value.
 */

#include <stdlib.h>
#include <string.h>

#include "tlv.h"

/* Largest payload a single TLV8 fragment can carry. A fragment of exactly this
 * many bytes means the value continues in the next same-type fragment. */
#define TLV_FRAGMENT_MAX 255u

/* Every emitted fragment costs a 1-byte type and 1-byte length header. */
#define TLV_HEADER_LEN 2u

/* Wipe a value buffer before releasing it: TLV values carry SRP and pairing
 * secrets that must not linger in freed heap memory. Safe on a NULL value. */
static void
tlv_free_value (uint8_t *value, size_t size)
{
  if (value == NULL)
    return;

  explicit_bzero (value, size);
  free (value);
}

/* ------------------------------------------------------------------ */
/* Container lifecycle                                                 */
/* ------------------------------------------------------------------ */

tlv_values_t *
tlv_new (void)
{
  tlv_values_t *values = calloc (1, sizeof *values);

  /* calloc already zeroed ->head and ->tail; nothing else to initialise. */
  return values;
}

void
tlv_free (tlv_values_t *values)
{
  tlv_t *node;

  if (values == NULL)
    return;

  node = values->head;
  while (node != NULL)
    {
      tlv_t *doomed = node;

      node = node->next;
      tlv_free_value (doomed->value, doomed->size);
      free (doomed);
    }

  free (values);
}

/* ------------------------------------------------------------------ */
/* Building a value list                                              */
/* ------------------------------------------------------------------ */

/* Link a node onto the tail of the list, taking ownership of @value (which may
 * be NULL for an empty item). Insertion order is preserved because it maps
 * directly onto on-wire order, which some peers are sensitive to. */
static int
tlv_append_owned (tlv_values_t *values, uint8_t type, uint8_t *value, size_t size)
{
  tlv_t *node = malloc (sizeof *node);

  if (node == NULL)
    return TLV_ERROR_MEMORY;

  node->next = NULL;
  node->type = type;
  node->value = value;
  node->size = size;

  /* Attach at the tail in O(1); an empty list has no tail yet. */
  if (values->tail == NULL)
    values->head = node;
  else
    values->tail->next = node;
  values->tail = node;

  return 0;
}

int
tlv_add_value (tlv_values_t *values, uint8_t type, const uint8_t *value, size_t size)
{
  uint8_t *copy = NULL;
  int result;

  /* A NULL value can only mean an empty item; ignore any stray size so the
   * memcpy below is never handed a NULL source (undefined behaviour). */
  if (value == NULL)
    size = 0;

  /* Keep an independent copy so the caller's buffer need not outlive us. An
   * empty value carries no allocation (data stays NULL, size stays 0). */
  if (size > 0)
    {
      copy = malloc (size);
      if (copy == NULL)
        return TLV_ERROR_MEMORY;
      memcpy (copy, value, size);
    }

  result = tlv_append_owned (values, type, copy, size);
  if (result != 0)
    tlv_free_value (copy, size);

  return result;
}

tlv_t *
tlv_get_value (const tlv_values_t *values, uint8_t type)
{
  /* Return the first item of this type; absent type yields NULL. Repeated
   * fragments were already merged at parse time, so one node per type. */
  for (tlv_t *node = values->head; node != NULL; node = node->next)
    {
      if (node->type == type)
        return node;
    }

  return NULL;
}

/* ------------------------------------------------------------------ */
/* Serialisation                                                      */
/* ------------------------------------------------------------------ */

/* Number of fragments needed to carry @size payload bytes. A non-empty value
 * splits into ceil(size / 255) fragments; an empty value still occupies one
 * (zero-length) fragment so the item survives the round trip. */
static size_t
tlv_fragment_count (size_t size)
{
  if (size == 0)
    return 1;

  return (size + (TLV_FRAGMENT_MAX - 1)) / TLV_FRAGMENT_MAX;
}

/* Total encoded byte count for one value (headers + payload). */
static size_t
tlv_encoded_len (size_t size)
{
  return size + TLV_HEADER_LEN * tlv_fragment_count (size);
}

int
tlv_format (const tlv_values_t *values, uint8_t *buffer, size_t *size)
{
  size_t needed = 0;
  uint8_t *out;

  /* First tally the exact size so the caller can allocate precisely (the
   * documented two-call sizing convention). */
  for (tlv_t *node = values->head; node != NULL; node = node->next)
    needed += tlv_encoded_len (node->size);

  if (buffer == NULL || *size < needed)
    {
      *size = needed;
      return TLV_ERROR_INSUFFICIENT_SIZE;
    }

  /* Second pass: emit each value as one or more <=255-byte fragments. */
  out = buffer;
  for (tlv_t *node = values->head; node != NULL; node = node->next)
    {
      const uint8_t *payload = node->value;
      size_t left = node->size;

      /* Empty value: a bare header with a zero length byte. */
      if (left == 0)
        {
          *out++ = node->type;
          *out++ = 0;
          continue;
        }

      while (left > 0)
        {
          size_t chunk = (left > TLV_FRAGMENT_MAX) ? TLV_FRAGMENT_MAX : left;

          *out++ = node->type;
          *out++ = (uint8_t) chunk;
          memcpy (out, payload, chunk);
          out += chunk;
          payload += chunk;
          left -= chunk;
        }
    }

  *size = needed;
  return 0;
}

/* ------------------------------------------------------------------ */
/* Parsing                                                            */
/* ------------------------------------------------------------------ */

/* Walk one run of consecutive same-@type fragments beginning at @start.
 *
 * The run ends at the first fragment shorter than 255 bytes (the "no more
 * follows" marker), at a differing type byte, or at end of input. When @dst is
 * non-NULL the concatenated payload is copied into it (the caller must have
 * sized it from a prior measuring pass). On return *end points just past the
 * run and *run_len holds the total payload length.
 *
 * Returns 0 on success or TLV_ERROR_INSUFFICIENT_SIZE if a fragment's declared
 * length would read past the end of the buffer. */
static int
tlv_walk_run (const uint8_t *buffer, size_t length, size_t start,
              uint8_t type, uint8_t *dst, size_t *run_len, size_t *end)
{
  size_t pos = start;
  size_t total = 0;

  /* A header needs two bytes; the run also stops at a foreign type byte. */
  while (pos + TLV_HEADER_LEN <= length && buffer[pos] == type)
    {
      size_t chunk = buffer[pos + 1];

      /* Reject a fragment that claims more payload than the buffer holds. */
      if (pos + TLV_HEADER_LEN + chunk > length)
        return TLV_ERROR_INSUFFICIENT_SIZE;

      if (dst != NULL && chunk > 0)
        {
          memcpy (dst + total, buffer + pos + TLV_HEADER_LEN, chunk);
        }

      total += chunk;
      pos += TLV_HEADER_LEN + chunk;

      /* Anything shorter than a full fragment terminates the value. */
      if (chunk < TLV_FRAGMENT_MAX)
        break;
    }

  *run_len = total;
  *end = pos;
  return 0;
}

int
tlv_parse (const uint8_t *buffer, size_t length, tlv_values_t *values)
{
  size_t pos = 0;

  while (pos < length)
    {
      uint8_t type;
      uint8_t *payload = NULL;
      size_t run_len = 0;
      size_t next = pos;
      int result;

      /* A lone trailing byte cannot start a fragment header; treat the input
       * as cleanly finished rather than reading past its end. */
      if (pos + TLV_HEADER_LEN > length)
        break;

      type = buffer[pos];

      /* Measure the full run first so we can allocate it in one block. */
      result = tlv_walk_run (buffer, length, pos, type, NULL, &run_len, &next);
      if (result != 0)
        return result;

      if (run_len > 0)
        {
          payload = malloc (run_len);
          if (payload == NULL)
            return TLV_ERROR_MEMORY;

          /* Second walk copies the fragments into the merged buffer. The
           * bounds were validated above, so this pass cannot fail. */
          result = tlv_walk_run (buffer, length, pos, type, payload, &run_len, &next);
          if (result != 0)
            {
              tlv_free_value (payload, run_len);
              return result;
            }
        }

      result = tlv_append_owned (values, type, payload, run_len);
      if (result != 0)
        {
          tlv_free_value (payload, run_len);
          return result;
        }

      pos = next;
    }

  return 0;
}
