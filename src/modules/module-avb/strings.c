/* SPDX-FileCopyrightText: Copyright © 2026 Nils Tonnaett
 * Nils Tonnaett <ntonnatt@ccrma.stanford.edu> */
/* SPDX-License-Identifier: MIT */

#include "strings.h"

typedef enum {
  ST_START,
  ST_A,
  ST_B,
  ST_C,
  ST_D,
  ST_E,
  ST_F,
  ST_G,
  ST_ERROR,
} UTF8_STATE;

int validate_utf8(uint8_t *str, size_t len) {
  UTF8_STATE state = ST_START;

  for (int i = 0; i < len; ++i) {
    switch (state) {
    case ST_START:
      if (str[i] <= 0x7F) {
        continue;
      } else if (str[i] >= 0xC2 && str[i] <= 0xDF) {
        state = ST_A;
      } else if (str[i] >= 0xE1 && str[i] <= 0xEC) {
        state = ST_B;
      } else if (str[i] >= 0xEE && str[i] <= 0xEF) {
        state = ST_B;
      } else if (str[i] == 0xE0) {
        state = ST_C;
      } else if (str[i] == 0xED) {
        state = ST_D;
      } else if (str[i] >= 0xF1 && str[i] <= 0xF3) {
        state = ST_E;
      } else if (str[i] == 0xF0) {
        state = ST_F;
      } else if (str[i] >= 0xF4) {
        state = ST_G;
      } else {
        state = ST_ERROR;
      }
      break;
    case ST_A:
      state = (str[i] >= 0x80 && str[i] <= 0xBF) ? ST_START : ST_ERROR;
      break;
    case ST_B:
      state = (str[i] >= 0x80 && str[i] <= 0xBF) ? ST_A : ST_ERROR;
      break;
    case ST_C:
      state = (str[i] >= 0xA0 && str[i] <= 0xBF) ? ST_A : ST_ERROR;
      break;
    case ST_D:
      state = (str[i] >= 0x80 && str[i] <= 0x9F) ? ST_A : ST_ERROR;
      break;
    case ST_E:
      state = (str[i] >= 0x80 && str[i] <= 0xBF) ? ST_B : ST_ERROR;
      break;
    case ST_F:
      state = (str[i] >= 0x90 && str[i] <= 0xBF) ? ST_B : ST_ERROR;
      break;
    case ST_G:
      state = (str[i] >= 0x80 && str[i] <= 0x8F) ? ST_B : ST_ERROR;
      break;
    }
    if (state == ST_ERROR) {
      return -1;
    }
  }
  if (state != ST_START) {
    return -1;
  } else {
    return 0;
  }
}
