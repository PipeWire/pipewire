/* AVB support */
/* SPDX-FileCopyrightText: Copyright © 2025 Kebag-Logic */
/* SPDX-FileCopyrightText: Copyright © 2025 Alex Malki <alexandre.malki@kebag-logic.com> */
/* SPDX-License-Identifier: MIT  */

#ifndef __GPTP_TLV_H__
#define __GPTP_TLV_H__

#include <stdint.h>
#include "gptp-defs.h"

/** @ see also {{@73#bkmrk-page-title}} */

struct gptp_tlv_base {
    uint16_t tlv_type;
    uint16_t tlv_length;
} __attribute__ ((__packed__));

struct gptp_tlv_as_path {
    struct gptp_tlv_base tlv;
    uint64_t tlv_data[0];
} __attribute__ ((__packed__));

#endif // __GPTP_TLV_H__
