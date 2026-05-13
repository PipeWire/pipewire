/* SPDX-FileCopyrightText: Copyright © 2026 Nils Tonnaett*/
/* SPDX-License-Identifier: MIT */

#ifndef AVB_STRINGS_H
#define AVB_STRINGS_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

int validate_utf8(uint8_t *str, size_t len);
int check_zero_padding(uint8_t *str, size_t len);

#endif /* AVB_STRINGS_H */
