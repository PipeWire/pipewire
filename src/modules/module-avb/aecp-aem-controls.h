/* AVB support */
/* SPDX-FileCopyrightText: Copyright © 2025 Kebag-Logic */
/* SPDX-FileCopyrightText: Copyright © 2025 Alexandre Malki <alexandre.malki@kebag-logic.com> */
/* SPDX-FileCopyrightText: Copyright © 2025 Simon Gapp <simon.gapp@kebag-logic.com> */
/* SPDX-License-Identifier: MIT */

#ifndef __AECP_AEM_CONTROLS_H__
#define __AECP_AEM_CONTROLS_H__

// TODO, When all the AVB needs to be supported then addition needs to be don here
/* IEEE 1722.1-2021, Table 7-121 - Value Types*/
#define AECP_AEM_CTRL_LINEAR_INT8             0x0000
#define AECP_AEM_CTRL_LINEAR_UINT8            0x0001
#define AECP_AEM_CTRL_LINEAR_INT16            0x0002
#define AECP_AEM_CTRL_LINEAR_UINT16           0x0003
#define AECP_AEM_CTRL_LINEAR_INT32            0x0004
#define AECP_AEM_CTRL_LINEAR_UINT32           0x0005
#define AECP_AEM_CTRL_LINEAR_INT64            0x0006
#define AECP_AEM_CTRL_LINEAR_UINT64           0x0007
#define AECP_AEM_CTRL_LINEAR_FLOAT            0x0008
#define AECP_AEM_CTRL_LINEAR_DOUBLE           0x0009

#define AECP_AEM_CTRL_SELECTOR_INT8           0x000a
#define AECP_AEM_CTRL_SELECTOR_UINT8          0x000b
#define AECP_AEM_CTRL_SELECTOR_INT16          0x000c
#define AECP_AEM_CTRL_SELECTOR_UINT16         0x000d
#define AECP_AEM_CTRL_SELECTOR_INT32          0x000e
#define AECP_AEM_CTRL_SELECTOR_UINT32         0x000f
#define AECP_AEM_CTRL_SELECTOR_INT64          0x0010
#define AECP_AEM_CTRL_SELECTOR_UINT64         0x0011
#define AECP_AEM_CTRL_SELECTOR_FLOAT          0x0012
#define AECP_AEM_CTRL_SELECTOR_DOUBLE         0x0013
#define AECP_AEM_CTRL_SELECTOR_STRING         0x0014

#define AEPC_AEM_CTRL_ARRAY_INT8              0x0015
#define AEPC_AEM_CTRL_ARRAY_UINT8             0x0016
#define AEPC_AEM_CTRL_ARRAY_INT16             0x0017
#define AEPC_AEM_CTRL_ARRAY_UINT16            0x0018
#define AEPC_AEM_CTRL_ARRAY_INT32             0x0019
#define AEPC_AEM_CTRL_ARRAY_UINT32            0x001a
#define AEPC_AEM_CTRL_ARRAY_INT64             0x001b
#define AEPC_AEM_CTRL_ARRAY_UINT64            0x001c
#define AEPC_AEM_CTRL_ARRAY_FLOAT             0x001d
#define AEPC_AEM_CTRL_ARRAY_DOUBLE            0x001e

#define AECP_AEM_CTRL_UTF8                    0x001f
#define AECP_AEM_CTRL_BODE_PLOT               0x0020
#define AECP_AEM_CTRL_SMPTE_TIME              0x0021
#define AECP_AEM_CTRL_SAMPLE_RATE             0x0022
#define AECP_AEM_CTRL_GPTP_TIME               0x0023

#define AECP_AEM_CTRL_CTRL_VENDOR          0x3ffe

/* Definition of the UNIT codes */
/* IEEE 1722.1-2021, Table 7-75 - Codes for Unitless quantities*/
#define AECP_AEM_CTRL_UNIT_CODE_UNITLESS      (0)
#define AECP_AEM_CTRL_UNIT_CODE_COUNT         (1)
#define AECP_AEM_CTRL_UNIT_CODE_PERCENT       (2)
#define AECP_AEM_CTRL_UNIT_CODE_FSTOP         (3)       



#define AECP_AEM_CTRL_FORMAT_VENDOR           (0)
#define AECP_AEM_CTRL_FORMAT_AVDECC           (1)

/* Identify
* IEEE 1722.1, Sec. 7.3.5.2 - Identify Control (IDENTIFY)
* Milan v1.2, Sec. 5.4.5.4 - Identification notification  
*/
#define AECP_AEM_CTRL_IDENTIFY_UNIT_MULTIPLY  0
#define AECP_AEM_CTRL_IDENTIFY_UNIT_CODE      AECP_AEM_CTRL_UNIT_CODE_UNITLESS
#define AECP_AEM_CTRL_IDENTIFY_STEP           (255)
#define AECP_AEM_CTRL_IDENTIFY_MINIMUM        (0)
#define AECP_AEM_CTRL_IDENTIFY_MAXIMUM        (255)


#define BASE_CTRL_TYPE_MAC            { 0x90, 0xe0, 0xf0, 0x01, 0x00, 0x00 };
// TODO AVB, for now support limited to Milan
#define BASE_CTRL_IDENTIFY_MAC        { 0x90, 0xe0, 0xf0, 0x00, 0x00, 0x01 };

#endif //__AECP_AEM_CONTROLS_H__